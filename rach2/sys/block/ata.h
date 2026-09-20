#ifndef _ATA_H_
#define _ATA_H_

#include "../lib/kint.h"

/**
 * Probe both ATA channels and register each disk that answers IDENTIFY.
 *
 * The first disk supplies `block0` if it is unset. Transfers use bus-master
 * DMA when the controller and drive support it, unless `rach2.atapio` selects
 * PIO. Waits sleep in a syscall with interrupts enabled and the scheduler
 * running, and poll otherwise, including during boot. Returns 0 when at least
 * one disk answered, or -1 when none did; an existing `block0` is preserved.
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
