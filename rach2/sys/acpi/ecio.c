/*
 * ecio.c: the embedded controller's command protocol over its two ports
 * (ecio.h), with a limit on each wait for the EC.
 */

#include "ecio.h"

#include "../lib/kerrno.h"

uint8_t ec_status(const struct ec_io *io)
{
    return io->in(io->command);
}

/**
 * Wait until the status has the bits `mask` equal to `want`, reading it
 * every EC_POLL_US for `timeout_us` at most: 0, or -ETIMEDOUT. The status is
 * read once more at the limit, so a wait of 0 still reads it.
 */
static int ec_wait(const struct ec_io *io, uint8_t mask, uint8_t want)
{
    uint32_t waited;

    for (waited = 0; waited < io->timeout_us; waited += EC_POLL_US) {
        if ((ec_status(io) & mask) == want) {
            return 0;
        }
        io->delay(EC_POLL_US);
    }
    return (ec_status(io) & mask) == want ? 0 : -ETIMEDOUT;
}

/** Write `value` to `port` once the EC has taken the byte before. */
static int ec_put(const struct ec_io *io, uint16_t port, uint8_t value)
{
    if (ec_wait(io, EC_IBF, 0) != 0) {
        return -ETIMEDOUT;
    }
    io->out(port, value);
    return 0;
}

/** Read the data port once the EC has put a byte out. */
static int ec_get(const struct ec_io *io, uint8_t *value)
{
    if (ec_wait(io, EC_OBF, EC_OBF) != 0) {
        return -ETIMEDOUT;
    }
    *value = io->in(io->data);
    return 0;
}

int ec_read(const struct ec_io *io, uint8_t address, uint8_t *value)
{
    if (ec_put(io, io->command, EC_READ) != 0 || ec_put(io, io->data, address) != 0) {
        return -ETIMEDOUT;
    }
    return ec_get(io, value);
}

/** The EC has the command once it takes the last byte, and the write is not
 * done before then. */
int ec_write(const struct ec_io *io, uint8_t address, uint8_t value)
{
    if (ec_put(io, io->command, EC_WRITE) != 0 || ec_put(io, io->data, address) != 0 ||
        ec_put(io, io->data, value) != 0) {
        return -ETIMEDOUT;
    }
    return ec_wait(io, EC_IBF, 0);
}

int ec_query(const struct ec_io *io, uint8_t *query)
{
    if (ec_put(io, io->command, EC_QUERY) != 0) {
        return -ETIMEDOUT;
    }
    return ec_get(io, query);
}
