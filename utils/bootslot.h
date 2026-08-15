#ifndef BOOTSLOT_H
#define BOOTSLOT_H

#include <stddef.h>
#include <stdint.h>

/*
 * bootslot: the install-kernel core. Everything that reads or writes the boot
 * partition lives here, behind caller-supplied sector callbacks, so the host
 * seam test drives the same code over image files with fault injection while
 * the guest main binds it to /dev/bootpart. The library validates the FAT32
 * volume before trusting one field of it, walks the namespace for the three
 * files the install contract names (/boot/kernel, /boot/fallback,
 * /boot/grub/grubenv), refuses every malformed shape by name, and performs the
 * install in the interruption-matrix order documented in
 * dev-log/23-install/plan.md.
 */

#define BOOTSLOT_SECTOR 512

/* Bounds on what the walk accepts: enough for the real 16 MiB boot partition
 * (252-sector FATs, 512-byte clusters) with generous headroom, small enough
 * that every array here is a plain static allocation. */
#define BOOTSLOT_FAT_MAX_SECTORS   512
#define BOOTSLOT_MAX_CLUSTERS      65536
#define BOOTSLOT_MAX_FILE_CLUSTERS 8192

/** Sector I/O over the boot partition, supplied by the caller. `lba` is
 *  partition-relative; both return 0 or -1. */
struct bootslot_io {
    int (*read)(void *ctx, uint32_t lba, void *buf);
    int (*write)(void *ctx, uint32_t lba, const void *buf);
    void *ctx;
};

/** One resolved file: its directory size and its walked cluster chain. */
struct bootslot_file {
    uint32_t size;
    uint32_t chain_len;
    uint32_t chain[BOOTSLOT_MAX_FILE_CLUSTERS];
};

/** The opened volume: validated geometry, one FAT, the three files. */
struct bootslot {
    struct bootslot_io io;
    uint32_t total_sectors;
    uint32_t cluster_sectors;
    uint32_t data_start;
    uint32_t cluster_count;
    uint32_t fat_start;
    uint32_t fat_sectors;
    uint8_t fat[BOOTSLOT_FAT_MAX_SECTORS * BOOTSLOT_SECTOR];
    struct bootslot_file kernel;
    struct bootslot_file fallback;
    struct bootslot_file grubenv;
    uint8_t visited[BOOTSLOT_MAX_CLUSTERS / 8];
};

/*
 * Every entry point returns 0 on success or -1 with one lowercase diagnostic
 * in `err` (always NUL-terminated, `errlen` its capacity) naming the cause.
 */

/** Read and validate the volume: the BPB, both FATs (compared whole), and the
 *  namespace walk that resolves the three files and requires every reachable
 *  chain legal, loop-free and disjoint. */
int bootslot_open(struct bootslot *bs, const struct bootslot_io *io, char *err, size_t errlen);

/** Validate `image` as a bootable kernel: ELF32 `ET_EXEC` for `EM_386`, the
 *  multiboot header four-byte-aligned in the first 8192 bytes with a checksum
 *  that sums to zero, a loader-shaped program-header table whose loads stay
 *  inside the file and at or above the megabyte, and an entry point inside a
 *  load. `present` is how many bytes of `image` are in memory (a fallback
 *  check reads only the head); ranges are judged against the full `size`.
 *  Pure. */
int bootslot_validate_kernel(const uint8_t *image, uint32_t present, uint32_t size, char *err,
                             size_t errlen);

/** Install `image` into the candidate slot, in the interruption-matrix order:
 *  validate the image; read and validate the fallback file; write
 *  `saved_entry=fallback`; write the slot cluster by cluster, zeros to its
 *  end; read the whole slot back and compare byte for byte; then write
 *  `next_entry=candidate`. */
int bootslot_install(struct bootslot *bs, const uint8_t *image, uint32_t size, char *err,
                     size_t errlen);

/** Write `saved_entry=candidate`, making the booted candidate the saved
 *  default. Requires the consumed-trial marker (`next_entry=`) in the block:
 *  an armed line means the slot was refilled after this boot, and no line at
 *  all means a fresh disk or an interrupted install, and neither state's
 *  slot bytes are the running kernel. */
int bootslot_commit(struct bootslot *bs, char *err, size_t errlen);

/** Read the environment block (1024 bytes) into `out`. */
int bootslot_read_env(struct bootslot *bs, uint8_t *out, char *err, size_t errlen);

#endif /* BOOTSLOT_H */
