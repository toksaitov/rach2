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
 * This phase is discovery only: it finds and maps the HBA and dumps its
 * registers. Later phases bring up a port, issue polled DMA, and publish
 * `block0`.
 */

/**
 * Discover the first AHCI HBA on the PCI bus and map its register block (ABAR).
 *
 * Returns 0 when an AHCI controller answered and its ABAR is mapped, or -1 when
 * none is present. Polled and IRQ-free, so it is safe to call before the
 * scheduler runs.
 */
int ahci_init(void);

/** Dump the discovered HBA's global and per-port registers to serial. */
void ahci_dump(void);

#endif
