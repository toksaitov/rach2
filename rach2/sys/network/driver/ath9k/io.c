/*
 * io.c: the ath9k driver's register helpers, written once over io.h's
 * indirection so that every part and every host test shares them.
 */

#include "ath9k.h"
#include "../../../lib/kerrno.h"

/**
 * Read `reg`, clear the bits of `clr`, set those of `set` and write the
 * result back. Returns the value written. The read and the write are two
 * accesses: a caller that races the interrupt handler on one register
 * holds the kernel lock across both.
 */
uint32_t ath9k_rmw(struct ath9k *ah, uint32_t reg, uint32_t set, uint32_t clr)
{
    uint32_t value = REG_READ(ah, reg);

    value &= ~clr;
    value |= set;
    REG_WRITE(ah, reg, value);
    return value;
}

/**
 * Poll `reg` until its bits under `mask` equal `value`, reading it
 * timeout / ATH9K_WAIT_QUANTUM times and at least once. Returns 0 or
 * -ETIMEDOUT.
 */
int ath9k_wait(struct ath9k *ah, uint32_t reg, uint32_t mask, uint32_t value, uint32_t timeout)
{
    uint32_t i, polls = timeout / ATH9K_WAIT_QUANTUM;

    if (polls == 0)
        polls = 1;
    for (i = 0; i < polls; i++) {
        if ((REG_READ(ah, reg) & mask) == value)
            return 0;
        ATH9K_UDELAY(ah, ATH9K_WAIT_QUANTUM);
    }
    return -ETIMEDOUT;
}

/**
 * Write column `column` of each row of `ini` to the row's register, and
 * wait a microsecond after every 64th row.
 */
void ath9k_write_ini(struct ath9k *ah, const struct ath9k_ini *ini, unsigned column)
{
    unsigned r;

    if (column == 0 || column >= ini->columns)
        return;
    for (r = 0; r < ini->rows; r++) {
        const uint32_t *row = ini->words + r * ini->columns;

        REG_WRITE(ah, row[0], row[column]);
        if ((r + 1) % 64 == 0)
            ATH9K_UDELAY(ah, 1);
    }
}
