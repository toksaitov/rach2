/*
 * io.h: the ath9k driver's register access, behind an indirection that a
 * host test replaces with the chip model, as rtl8168.c's is.
 *
 * Every part of the driver reaches the chip through REG_READ and REG_WRITE
 * on its `struct ath9k`, which call the `ath9k_io` it holds. In the kernel
 * that is pci.c's ath9k_mmio_io, plain MMIO on the card's window and
 * k_udelay; in a host test it is tests/host/ath9k_model.h's
 * ath9k_model_io, which answers from a register file and records each
 * access in order. The macros' names and arguments are ath9k's (hw.h), so
 * that a sequence ported from ath9k reads as it does there.
 */
#ifndef _ATH9K_IO_H_
#define _ATH9K_IO_H_

#include "../../../lib/kint.h"

struct ath9k;

/**
 * A register window and its waits. `ctx` is the `io_ctx` of the
 * `struct ath9k` that holds the table. `udelay` waits at least `usec`
 * microseconds: every use is a poll interval or a settle time from ath9k,
 * and a longer wait is always safe. All three may be called with the
 * kernel lock held and from the card's interrupt handler; none sleeps.
 */
struct ath9k_io {
    uint32_t (*read)(void *ctx, uint32_t reg);
    void (*write)(void *ctx, uint32_t reg, uint32_t value);
    void (*udelay)(void *ctx, uint32_t usec);
};

/**
 * One of ath9k's initial-value tables: `rows` rows of `columns` words, the
 * register's offset first and its values after it. A two-column table
 * holds one value for every mode; a five-column one holds a value for each
 * of ath9k's modes, among them 2.4 GHz HT20.
 */
struct ath9k_ini {
    const uint32_t *words;
    unsigned rows;
    unsigned columns;
};

/* The ath9k_ini of a static two-dimensional table `t` of uint32_t. */
#define ATH9K_INI(t) {&(t)[0][0], sizeof(t) / sizeof((t)[0]), sizeof((t)[0]) / sizeof((t)[0][0])}

/* A field's value `v` shifted into mask `f`, and the value of field `f` of
 * register value `v`; `f##_S` is the field's shift. */
#define SM(v, f) (((uint32_t)(v) << f##_S) & (f))
#define MS(v, f) (((uint32_t)(v) & (f)) >> f##_S)

#define REG_READ(ah, reg)            ((ah)->io->read((ah)->io_ctx, (reg)))
#define REG_WRITE(ah, reg, value)    ((ah)->io->write((ah)->io_ctx, (reg), (value)))
#define REG_RMW(ah, reg, set, clr)   ath9k_rmw((ah), (reg), (set), (clr))
#define REG_SET_BIT(ah, reg, f)      ath9k_rmw((ah), (reg), (f), 0)
#define REG_CLR_BIT(ah, reg, f)      ath9k_rmw((ah), (reg), 0, (f))
/* These two paste f##_S themselves: passed on to SM or MS, the field's name
 * would be replaced by its value before the paste. */
#define REG_RMW_FIELD(ah, reg, f, v) ath9k_rmw((ah), (reg), ((uint32_t)(v) << f##_S) & (f), (f))
#define REG_READ_FIELD(ah, reg, f)   ((REG_READ((ah), (reg)) & (f)) >> f##_S)
#define ATH9K_UDELAY(ah, usec)       ((ah)->io->udelay((ah)->io_ctx, (usec)))

/* ath9k's poll interval and its usual bound, in microseconds (hw.h's
 * AH_TIME_QUANTUM and AH_WAIT_TIMEOUT). */
#define ATH9K_WAIT_QUANTUM 10
#define ATH9K_WAIT_TIMEOUT 100000

/**
 * Read `reg`, clear the bits of `clr`, set those of `set` and write the
 * result back, as ath9k's ath9k_reg_rmw does. Returns the value written.
 */
uint32_t ath9k_rmw(struct ath9k *ah, uint32_t reg, uint32_t set, uint32_t clr);

/**
 * Poll `reg` every ATH9K_WAIT_QUANTUM microseconds until its bits under
 * `mask` equal `value`, for at most `timeout` microseconds, as ath9k's
 * ath9k_hw_wait does: the register is read timeout / ATH9K_WAIT_QUANTUM
 * times, at least once. Returns 0 when it matched, or -ETIMEDOUT; it logs
 * nothing, so that the caller names the step that failed.
 */
int ath9k_wait(struct ath9k *ah, uint32_t reg, uint32_t mask, uint32_t value, uint32_t timeout);

/**
 * Write column `column` of each row of `ini` to the row's register, in the
 * table's order, as ath9k's ath9k_hw_write_array does, and wait a
 * microsecond after every 64th row, as its DO_DELAY does with a count that
 * runs on across tables. Column 0 is the offset; a column past the table's
 * last writes nothing.
 */
void ath9k_write_ini(struct ath9k *ah, const struct ath9k_ini *ini, unsigned column);

#endif
