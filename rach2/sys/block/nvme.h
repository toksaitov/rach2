/*
 * nvme.h: the NVMe driver: controllers on PCI (class 01, subclass 08,
 * interface 02), each with an admin queue and one I/O queue pair, and each
 * active namespace published as a disk of the block layer, as an AHCI disk
 * is.
 *
 * The boot drives the controllers by polling, as it drives AHCI: the reads
 * that mount the root run before the scheduler, and a page fault with
 * interrupts off may read or write a page at any time after. `nvme_start`
 * then moves the namespaces to requests completed by the controller's
 * message, MSI-X or MSI, and keeps the polled transfers for the fault path.
 * `sys/block/Readme.md` describes the queues, the two paths and the recovery.
 */
#ifndef _NVME_H_
#define _NVME_H_

#include "../lib/kint.h"

/**
 * Find every NVMe controller on the bus, up to four, bring each up and
 * register its namespaces with the disk registry, as transport `nvme` at the
 * controller's number times four plus the namespace's place, publishing the
 * first as `block0` when no other driver has.
 *
 * A controller already brought up by an earlier call is not reset again:
 * its namespaces may be mounted. Its disks are registered again, which
 * keeps their numbers. Returns 0 when at least one namespace is published,
 * -1 otherwise. Polled, so it may run before the scheduler; called from
 * `block_probe`.
 */
int nvme_init(void);

/**
 * Switch every controller with published namespaces to interrupt-driven
 * requests: a vector from `isr_alloc_vector`, the controller's MSI-X or MSI
 * message on it, and `submit` and `flush` on each namespace in place of
 * `BLOCK_FAULT_SAFE`. A controller keeps polling under `rach2.nvmepoll`,
 * without the local APIC, without a free vector, or without a message
 * capability, and logs which; in the last case it gives the vector back,
 * with its handler. Called once at boot, after the root is mounted, with
 * interrupts on.
 */
void nvme_start(void);

/** Messages the handler has taken, all controllers together. */
uint32_t nvme_interrupts(void);

/*
 * The controllers as this driver reaches them: a register, read and written
 * 32 bits at a time at an offset into the BAR0 mapped at `base`, and the
 * delay. A machine uses the mapped BARs and `k_udelay`; a host test
 * supplies a model of a controller, which is how a controller that never
 * becomes ready or a command that never completes is made to happen.
 */
struct nvme_io {
    uint32_t (*r32)(uintptr_t base, uint32_t offset);
    void (*w32)(uintptr_t base, uint32_t offset, uint32_t value);
    void (*udelay)(uint32_t microseconds);
};

/** Use `ops` instead of the mapped controller; NULL restores the machine's. */
void nvme_set_io(const struct nvme_io *ops);

#endif
