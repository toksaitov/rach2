#ifndef _AHCI_H_
#define _AHCI_H_

#include "../lib/kint.h"

/**
 * AHCI (SATA) disk driver, the modern sibling of the polled-PIO ATA driver
 * behind the block layer.
 *
 * Driven by polled completion, with no interrupt: the page-cache window fault
 * runs with interrupts off and cannot sleep, so a transfer spins on the
 * command-issue bit (`PxCI`) instead of waiting on the AHCI completion
 * interrupt. The HBA still does the DMA in hardware; the CPU only polls a
 * status bit. See `sys/block/Readme.md` for the rationale and the
 * polled-vs-IRQ-vs-async trade-off.
 *
 * `ahci_init` finds and maps the HBA, takes ownership, brings up the first
 * present port's command engine, sizes the disk with IDENTIFY, and publishes it
 * as `block0`. `block_probe` calls it when the `rach2.disk=ahci` boot flag is
 * set; otherwise the ATA driver wins `block0`.
 */

/**
 * Discover the first AHCI HBA, bring up its first present port, and publish the
 * disk as `block0` for the polled read/write path.
 *
 * Maps the register block (ABAR), takes ownership, starts the port engine,
 * sizes the device with IDENTIFY, and points `block0` at the AHCI backend.
 * Returns 0 when a disk came up and `block0` is set, or -1 otherwise. Polled
 * and IRQ-free, so it is safe to call before the scheduler runs.
 *
 * A failure after the port engine starts (a failed IDENTIFY) leaves that port
 * started but idle and `block0` unset, so `block_probe` falls back to ATA. The
 * idle port issues no DMA without a queued command (`PxCI` stays clear) and the
 * HBA's MMIO does not overlap the legacy ATA ports, so the inert HBA needs no
 * cleanup.
 */
int ahci_init(void);

/**
 * Switch the disks to the interrupt-driven path: the controller's messages
 * on an allocated vector, one command active per port and a queue behind
 * it. Called once interrupts are on and the scheduler runs; before that,
 * and under `rach2.ahcipoll`, every transfer is polled. A disk switched
 * loses `BLOCK_FAULT_SAFE` and gains `submit`; its polled `read` and
 * `write` remain for the fault path, which drains the port first.
 */
void ahci_start(void);

/** Dump the discovered HBA's global and per-port registers to serial. */
void ahci_dump(void);

/**
 * Self-test the polled path: read LBA 0 and report its MBR signature. Requires
 * a prior successful `ahci_init`, which has already sized the device with
 * IDENTIFY.
 */
void ahci_selftest(void);

/*
 * The controller as this driver reaches it: the two register accessors and the
 * delay. A machine uses the mapped ABAR and `k_udelay`; a host test supplies a
 * model of the controller instead, which is the only way to make a reset that
 * never completes or a command that times out happen on demand.
 */
struct ahci_io {
    uint32_t (*r32)(uint32_t offset);
    void (*w32)(uint32_t offset, uint32_t value);
    void (*udelay)(uint32_t microseconds);
};

/** Use `ops` instead of the mapped controller; NULL restores the machine's. */
void ahci_set_io(const struct ahci_io *ops);

#endif
