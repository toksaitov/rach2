/*
 * ahci.h: the AHCI driver for SATA disks: bring-up of the first controller in
 * AHCI mode, the move of its disks to interrupts, and the debug shell's dump.
 *
 * The boot drives the disks by polling: the reads that mount the root run
 * before the scheduler, and a page fault with interrupts off may read or
 * write a page at any time after, so a polled transfer spins on the
 * command-issue bit (`PxCI`) while the controller does the DMA. `ahci_start`
 * then moves the disks to requests completed by the controller's MSI
 * message, and keeps the polled transfers for the fault path.
 * `sys/block/Readme.md` describes both paths.
 */
#ifndef _AHCI_H_
#define _AHCI_H_

#include "../lib/kint.h"

/**
 * Bring up the first PCI controller in AHCI mode and register each disk on
 * it, up to four, as transport `ahci` at its port number.
 *
 * `block0` is cleared first and set to the first disk that answers
 * IDENTIFY, so a probe that publishes no disk leaves it NULL. The controller
 * is reset, and so is every implemented port; a port whose device does not
 * answer is stopped and left, and the others go on. Returns 0 when at least
 * one disk is published, or -1 otherwise, with the cause logged and, when a
 * controller was found, its registers dumped to the console. Polled, with
 * the controller's interrupts off, so it may run before the scheduler: from
 * `block_probe`, and from the debug shell's `ahci`, which withdraws the nodes
 * on the old `block0` first.
 */
int ahci_init(void);

/**
 * Move the disks to the interrupt-driven path: the controller's MSI message
 * on a vector of its own, one command active on each port and a queue behind
 * it. A disk moved loses `BLOCK_FAULT_SAFE` and gains `submit` and `flush`;
 * its polled `read` and `write` remain for the fault path, which finishes
 * the port's active command first. The disks stay polled under
 * `rach2.ahcipoll`, and, with a record, when there is no local APIC, no free
 * vector or no MSI capability; in the last case the vector is given back,
 * with its handler. Called once at boot, after the root is mounted, with
 * interrupts on.
 */
void ahci_start(void);

/**
 * Print the controller's registers to the console: the global ones, the
 * handoff and Intel port-enable words, each disk's DMA addresses, and each
 * implemented port's link, signature, task file, command and error registers.
 */
void ahci_dump(void);

/**
 * Read sector 0 of `block0` through its `read` and print whether it has the
 * MBR signature. For the debug shell, after a successful `ahci_init`, which
 * has made `block0` the controller's first disk.
 */
void ahci_selftest(void);

/*
 * The controller as this driver reaches it: the two register accessors and the
 * delay. A machine uses the mapped ABAR and `k_udelay`; a host test supplies a
 * model of the controller instead, which is the only way to make a reset that
 * never completes or a command that times out happen on demand. PCI
 * configuration space is reached directly and is not in the table; the e1000
 * driver has the same arrangement.
 */
struct ahci_io {
    uint32_t (*r32)(uint32_t offset);
    void (*w32)(uint32_t offset, uint32_t value);
    void (*udelay)(uint32_t microseconds);
};

/** Use `ops` in place of the mapped controller; NULL restores the machine's. */
void ahci_set_io(const struct ahci_io *ops);

#endif
