#ifndef BOOTSLOT_H
#define BOOTSLOT_H

#include <stddef.h>
#include <stdint.h>

/*
 * bootslot: the boot-slot core bootctl drives. Everything that reads or writes the boot
 * partition lives here, behind caller-supplied sector callbacks, so the host
 * seam test drives the same code over image files with fault injection while
 * the guest main binds it to /dev/bootpart. The library validates the FAT32
 * volume before trusting one field of it, walks the namespace for the four
 * files the install contract names (/boot/kernel, /boot/fallback,
 * /boot/grub/grubenv), refuses every malformed shape by name, and performs the
 * install in the interruption-matrix order documented in
 * dev-log/23-install/plan.md.
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

/* The longest menu id the install writes: `candidate-static` and its NUL. */
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
    int have_menu;      /* a disk built before the menu slot existed has none */
    int static_entries; /* the machine is running from the fixed-address pair */
    uint8_t visited[BOOTSLOT_MAX_CLUSTERS / 8];
};

/*
 * Every entry point returns 0 on success or -1 with one lowercase diagnostic
 * in `err` (always NUL-terminated, `errlen` its capacity) naming the cause.
 */

/**
 * Say that this machine is running from the fixed-address menu entries.
 *
 * The menu holds two pairs: `candidate` and `fallback` take whatever the
 * network gives them, and `candidate-static` and `fallback-static` carry a
 * fixed address for a network with no DHCP server. Which pair a machine is
 * running from is the caller's to determine (`bootctl` reads it off
 * `/proc/cmdline`), and it decides which ids the install arms and the commit
 * saves: a machine left on the fixed address must not be moved back to the
 * other pair by an install, since on that network the other pair comes up with
 * no address at all.
 */
void bootslot_use_static_entries(struct bootslot *bs, int on);

/** Read and validate the volume: the BPB, both FATs (compared whole), and the
 *  namespace walk that resolves the four files and requires every reachable
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

/**
 * Write `size` bytes of `text` into the menu slot, padding the rest.
 *
 * The slot is a fixed-size file preallocated at build time, so this overwrites
 * bytes in place and never extends a file or moves a cluster chain: the chain
 * was found once when the volume was opened and does not change. Every sector
 * is written whether the new text reaches it or not, so a shorter menu leaves
 * none of a longer one behind, and each is read back and compared before the
 * next, so a slot that does not take the write fails here rather than at the
 * next boot. The padding is '#' rather than zeros because GRUB reads the whole
 * file and a comment is what an empty tail has to look like to it.
 *
 * This is not atomic and cannot be: one slot overwritten in place can be left
 * holding a prefix of a new menu above the tail of an old one. What makes that
 * survivable is elsewhere, in the immutable stub, which defines every entry the
 * machine needs before it reads this file and never lets it choose the default.
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
 * the allowlist belongs with the tool a person types at rather than with the
 * library.
 */
int bootslot_env_set(struct bootslot *bs, const char *key, const char *value, char *err,
                     size_t errlen);

/**
 * Remove one key from the environment block, preserving every other line.
 *
 * Removing is not the same as setting empty, and for `next_entry` the
 * difference decides whether a kernel may be committed: an empty value is the
 * consumed marker GRUB's scripted consumption leaves after a trial boot, and
 * `bootslot_commit` requires it. A key that is simply absent is a machine with
 * no trial, which is what cancelling one means.
 */
int bootslot_env_drop(struct bootslot *bs, const char *key, char *err, size_t errlen);

/** Read the environment block (1024 bytes) into `out`. */
int bootslot_read_env(struct bootslot *bs, uint8_t *out, char *err, size_t errlen);

#endif /* BOOTSLOT_H */
