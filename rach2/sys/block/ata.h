#ifndef _ATA_H_
#define _ATA_H_

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

#endif
