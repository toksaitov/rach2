/*
 * io.c: the AR9565 driver's register helpers, written once over io.h's
 * indirection so that every part and every host test shares them.
 */

#include "ar9565.h"
#include "../../../lib/kerrno.h"

/**
 * Read `reg`, clear the bits of `clr`, set those of `set` and write the
 * result back. Returns the value written. The read and the write are two
 * accesses: a caller that races the interrupt handler on one register
 * holds the kernel lock across both.
 */
uint32_t ar9565_rmw(struct ar9565 *ah, uint32_t reg, uint32_t set, uint32_t clr)
{
    uint32_t value = REG_READ(ah, reg);

    value &= ~clr;
    value |= set;
    REG_WRITE(ah, reg, value);
    return value;
}

/**
 * Poll `reg` until its bits under `mask` equal `value`, reading it
 * timeout / AR9565_WAIT_QUANTUM times and at least once. Returns 0 or
 * -ETIMEDOUT.
 */
int ar9565_wait(struct ar9565 *ah, uint32_t reg, uint32_t mask, uint32_t value, uint32_t timeout)
{
    uint32_t i, polls = timeout / AR9565_WAIT_QUANTUM;

    if (polls == 0)
        polls = 1;
    for (i = 0; i < polls; i++) {
        if ((REG_READ(ah, reg) & mask) == value)
            return 0;
        AR9565_UDELAY(ah, AR9565_WAIT_QUANTUM);
    }
    return -ETIMEDOUT;
}

/**
 * Write column `column` of each row of `ini` to the row's register, and
 * wait a microsecond after every 64th row.
 */
void ar9565_write_ini(struct ar9565 *ah, const struct ar9565_ini *ini, unsigned column)
{
    unsigned r;

    if (column == 0 || column >= ini->columns)
        return;
    for (r = 0; r < ini->rows; r++) {
        const uint32_t *row = ini->words + r * ini->columns;

        REG_WRITE(ah, row[0], row[column]);
        if ((r + 1) % 64 == 0)
            AR9565_UDELAY(ah, 1);
    }
}
