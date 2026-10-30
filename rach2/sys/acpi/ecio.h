/*
 * ecio.h: the command protocol of an ACPI embedded controller over its two
 * ports, as the ACPI specification's chapter on the EC gives it, apart from
 * ACPICA so that a host test can drive it against a model of the ports.
 */
#ifndef _ACPI_ECIO_H_
#define _ACPI_ECIO_H_

#include "../lib/kint.h"

/* The bits of the status register, read from the command port. */
#define EC_OBF     0x01 /* the output buffer holds a byte for the host */
#define EC_IBF     0x02 /* the input buffer holds a byte the EC has not taken */
#define EC_CMD     0x08 /* the last byte written was a command */
#define EC_BURST   0x10
#define EC_SCI_EVT 0x20 /* the EC has a query for the host */
#define EC_SMI_EVT 0x40

/* The commands, written to the command port. */
#define EC_READ  0x80
#define EC_WRITE 0x81
#define EC_QUERY 0x84

/* The time a step of a command may wait for the EC, in microseconds, as
 * Linux's ec_delay of 500 ms, and how often the status is read meanwhile. */
#define EC_TIMEOUT_US 500000u
#define EC_POLL_US    10u

/*
 * An EC's ports and how to reach them: the kernel's inb and outb and
 * k_udelay, or a host test's model, which keeps its own clock. The caller
 * runs one command at a time on an EC.
 */
struct ec_io {
    uint16_t data;    /* the data port, 0x62 on a PC */
    uint16_t command; /* the command and status port, 0x66 */
    uint8_t (*in)(uint16_t port);
    void (*out)(uint16_t port, uint8_t value);
    void (*delay)(uint32_t us);
    uint32_t timeout_us; /* each step's limit, EC_TIMEOUT_US unless a test sets less */
};

/** The status register. */
uint8_t ec_status(const struct ec_io *io);

/**
 * Read the byte at `address` of the EC's space into `*value`: the command,
 * the address once the EC took the command, and the byte once the EC has
 * put it out. 0, or -ETIMEDOUT when the EC did not take a byte or give one
 * within `timeout_us` of a step, which leaves `*value` alone.
 */
int ec_read(const struct ec_io *io, uint8_t address, uint8_t *value);

/** Write `value` to `address` of the EC's space: the command, the address
 * and the byte, each once the EC took the one before. 0 or -ETIMEDOUT. */
int ec_write(const struct ec_io *io, uint8_t address, uint8_t value);

/**
 * Ask the EC which event it has, after it set SCI_EVT: the query command,
 * and the byte it puts out, the number xx of the method _Qxx to run, which
 * clears SCI_EVT. 0 with `*query` 0 when the EC had none. 0 or -ETIMEDOUT.
 */
int ec_query(const struct ec_io *io, uint8_t *query);

#endif /* _ACPI_ECIO_H_ */
