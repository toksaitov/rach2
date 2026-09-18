#ifndef _ATA_H_
#define _ATA_H_

#include "../lib/kint.h"

/**
 * Probes the primary ATA channel for a master disk and, on success, publishes
 * it as `block0`.
 *
 * Issues IDENTIFY, reads the capacity, and wires `block0` to the polled
 * read/write path. Returns 0 when a disk answered and `block0` is set, or -1
 * when none responded (then `block0` is left `NULL`). The whole path is polled
 * with no IRQ, so it is safe to call before the scheduler runs.
 */
int ata_init(void);

/** The tick's call: the channels' clocks and the sleepers' wake. */
void ata_tick(void);

/**
 * How the driver reaches a channel's ports: a byte in and out, the bus
 * master's descriptor address out, and a sector's words in and out through
 * the data port. The machine's are the processor's port instructions; a
 * host test's are a model of a drive.
 */
struct ata_io {
    uint8_t (*in8)(uint16_t port);
    void (*out8)(uint16_t port, uint8_t value);
    void (*out32)(uint16_t port, uint32_t value);
    void (*ins16)(uint16_t port, void *buffer, uint32_t words);
    void (*outs16)(uint16_t port, const void *buffer, uint32_t words);
};

/** Use `ops` instead of the machine's ports; NULL restores the machine's. */
void ata_set_io(const struct ata_io *ops);

#endif
