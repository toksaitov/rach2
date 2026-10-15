/*
 * ata.h: the legacy ATA driver: the probe of the IDE controller's two
 * channels, the tick that bounds its waits, and the port table a test
 * replaces.
 */
#ifndef _ATA_H_
#define _ATA_H_

#include "../lib/kint.h"

/**
 * Probe both channels of the IDE controller and register each disk that
 * answers IDENTIFY, as transport `ata` at channel times 2 plus drive.
 *
 * The first disk found becomes `block0` when no other driver has published
 * one; an existing `block0` is kept. A transfer goes through the bus master
 * when the controller has one and the drive does DMA, unless `rach2.atapio`
 * is on the boot line, and through the data port otherwise. A wait for the
 * drive sleeps inside a system call with interrupts on and the scheduler
 * running, and polls everywhere else, at boot and in a page fault among
 * them. Called from `platform_disks_probe` by each probe of the disks.
 * Returns 0 when at least one disk answered, or -1 when none did.
 */
int ata_init(void);

/**
 * The timer's call, every tick, from its interrupt: advance each channel's
 * clock and wake the callers asleep on it, so that a wait for an interrupt
 * the drive never raises ends.
 */
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

/** Use `ops` in place of the machine's ports; NULL restores the machine's. */
void ata_set_io(const struct ata_io *ops);

#endif
