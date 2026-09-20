#ifndef BOOTSLOT_H
#define BOOTSLOT_H

#include <stddef.h>
#include <stdint.h>

/*
 * bootslot: the boot-slot core bootctl drives. Everything that reads or
 * writes the boot partition lives here, behind caller-supplied sector
 * callbacks, so the host test drives the same code over image files
 * with fault injection while the guest main binds it to /dev/bootpart. The
 * library validates the FAT32 volume before trusting one field of it, walks
 * the namespace for the four files the install contract names (/boot/kernel,
 * /boot/fallback, /boot/grub/grubenv), refuses every malformed shape by
 * name, and performs the install in the interruption-matrix order documented
 * in dev-log/23-install/plan.md.
 */

#define BOOTSLOT_SECTOR 512

/* GRUB's environment block is exactly this many bytes and is never resized;
 * `bootslot_read_env` fills that many, so a caller sizes its buffer from it. */
#define BOOTSLOT_ENV_BYTES 1024

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

/* Room for the longest menu id an install writes, `candidate`, and its NUL,
   with the margin a longer id would need; the block's keys are short. */
#define BOOTSLOT_ENTRY_MAX 24

/** The opened volume: validated geometry, one FAT, the four files. */
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
    struct bootslot_file menu;
    int have_menu; /* a disk built before the menu slot existed has none */
    uint8_t visited[BOOTSLOT_MAX_CLUSTERS / 8];
};

/*
 * Every entry point returns 0 on success or -1 with one lowercase diagnostic
 * in `err` (always NUL-terminated, `errlen` its capacity) naming the cause.
 */

/** Read and validate the volume: the BPB, both FATs (compared whole), and the
 *  namespace walk that resolves the four files and requires every reachable
 *  chain legal, loop-free and disjoint. */
int bootslot_open(struct bootslot *bs, const struct bootslot_io *io, char *err, size_t errlen);

/**
 * Validate ELF32 i386 kernel headers without changing the image.
 *
 * Load file ranges must fit `size`. Load memory ranges start at or above
 * 1 MiB and have a representable 32-bit exclusive end. ELF and program
 * headers must fit `present`, the available prefix. The entry point must
 * lie in a load's file-backed bytes. A four-byte-aligned Multiboot header
 * in the first 8192 bytes must have ALIGN|MEMINFO flags and a valid checksum.
 */
int bootslot_validate_kernel(const uint8_t *image, uint32_t present, uint32_t size, char *err,
                             size_t errlen);

/** Install `image` into the candidate slot, in the interruption-matrix order:
 *  validate the image; read and validate the fallback file; write
 *  `saved_entry=fallback`; write the slot cluster by cluster, zeros to its
 *  end; read the whole slot back and compare byte for byte; then write
 *  `next_entry=candidate`. */
int bootslot_install(struct bootslot *bs, const uint8_t *image, uint32_t size, char *err,
                     size_t errlen);

/**
 * Write `saved_entry=candidate` when `next_entry` is present and empty.
 *
 * GRUB saves this marker before loading the trial kernel; it does not prove
 * a completed boot. The caller must check `rach2.slot=candidate`.
 */
int bootslot_commit(struct bootslot *bs, char *err, size_t errlen);

/**
 * Copy and verify the candidate slot as the fallback, restoring its header
 * only after the remaining sectors have been verified.
 *
 * Require `saved_entry=candidate`, no nonempty `next_entry`, and a fallback
 * file at least as long as the candidate slot. These environment values do
 * not establish which kernel ran. A failed final write may leave a partial
 * header. Boot the copied fallback once before relying on it.
 */
int bootslot_fallback(struct bootslot *bs, char *err, size_t errlen);

/**
 * Write `size` bytes of `text` into the menu slot, padding the rest.
 *
 * The slot is a fixed-size file preallocated at build time, so this overwrites
 * bytes in place and never extends a file or moves a cluster chain: the chain
 * was found once when the volume was opened and does not change. Every sector
 * is written whether the new text reaches it or not, so a shorter menu leaves
 * none of a longer one behind, and each is read back and compared before the
 * next, so a slot that does not take the write fails here, not at the
 * next boot. The padding is '#', not zeros because GRUB reads the whole
 * file and a comment is what an empty tail has to look like to it.
 *
 * An interrupted write can leave a new prefix above an old tail. The stub
 * defines the required entries before sourcing this file. The file is
 * unrestricted GRUB script and can change the variables used to select the
 * default or boot a kernel directly.
 *
 * Refuses text larger than the slot, and a disk that has no slot at all,
 * without writing anything.
 */
int bootslot_write_menu(struct bootslot *bs, const uint8_t *text, uint32_t size, char *err,
                        size_t errlen);

/**
 * Set one key in the environment block, preserving every other line.
 *
 * The block is 1024 bytes with no checksum, which GRUB parses at boot with no
 * recourse if it is malformed, so the write is read back and compared. This
 * imposes no policy on which keys may be set; the caller decides that, because
 * the allowlist belongs with the tool a person types at, not with the
 * library.
 */
int bootslot_env_set(struct bootslot *bs, const char *key, const char *value, char *err,
                     size_t errlen);

/**
 * Remove one key from the environment block, preserving every other line.
 *
 * An empty `next_entry` is the marker `bootslot_commit` requires. GRUB saves
 * it before loading a trial kernel. Removing the key cancels the trial
 * without supplying that marker.
 */
int bootslot_env_drop(struct bootslot *bs, const char *key, char *err, size_t errlen);

/** Read the environment block (1024 bytes) into `out`. */
int bootslot_read_env(struct bootslot *bs, uint8_t *out, char *err, size_t errlen);

#endif /* BOOTSLOT_H */
