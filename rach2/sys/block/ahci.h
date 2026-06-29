#ifndef _AHCI_H_
#define _AHCI_H_

/**
 * AHCI (SATA) disk driver, the modern sibling of the polled-PIO ATA driver
 * behind the block layer.
 *
 * Driven by polled completion, with no interrupt: the page-cache window fault
 * runs with interrupts off and cannot sleep, so a transfer spins on the
 * command-issue bit (`PxCI`) rather than waiting on the AHCI completion
 * interrupt. The HBA still does the DMA in hardware; the CPU only polls a status
 * bit. See `sys/block/Readme.md` for the rationale and the polled-vs-IRQ-vs-async
 * trade-off.
 *
 * `ahci_init` finds and maps the HBA, takes ownership, and brings up the first
 * present port's command engine. A later phase publishes `block0`.
 */

/**
 * Discover the first AHCI HBA, map its register block (ABAR), and bring up the
 * first present port for polled command issue.
 *
 * Returns 0 when an AHCI controller answered, its ABAR is mapped, and a port
 * with a device came up, or -1 otherwise. Polled and IRQ-free, so it is safe to
 * call before the scheduler runs.
 */
int ahci_init(void);

/** Dump the discovered HBA's global and per-port registers to serial. */
void ahci_dump(void);

/**
 * Self-test the polled path: IDENTIFY the device, then read LBA 0 and report its
 * MBR signature. Requires a prior successful `ahci_init`.
 */
void ahci_selftest(void);

#endif
