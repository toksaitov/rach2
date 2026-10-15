#ifndef BOOTSLOT_H
#define BOOTSLOT_H

#include <stddef.h>
#include <stdint.h>

/*
 * bootslot: the kernel slots, the menu slot and GRUB's environment block of
 * the boot partition's FAT32 volume, read and written through sector
 * callbacks the caller supplies.
 *
 * bootctl binds the callbacks to the boot partition's node, growboot to a
 * backup of the partition in a file, and the host test to an image in
 * memory, where a write can be dropped, torn or stopped. `bootslot_open`
 * checks the volume before it uses a field of it and finds the four files
 * of the install contract: /boot/kernel, /boot/fallback, /boot/grub/grubenv
 * and /boot/boot.cfg, the last of which a volume may lack. A malformed
 * volume fails with a diagnostic that names its fault, and an install
 * writes in the order of the interruption matrix in
 * dev-log/23-install/plan.md.
 */

/* The sector size the volume must have and the callbacks move. */
#define BOOTSLOT_SECTOR 512

/* The size of GRUB's environment block, which `bootslot_open` requires of
 * /boot/grub/grubenv and which neither GRUB nor this library changes;
 * `bootslot_read_env` fills that many bytes, so a caller sizes its buffer
 * from it. */
#define BOOTSLOT_ENV_BYTES 1024

/* Bounds on what the walk accepts: enough for an EFI system partition of up
 * to 64 MiB at 512-byte clusters (1016-sector FATs, about 130000 clusters),
 * since UEFI firmware reads a FAT32 volume only when it has at least 65525
 * clusters, and for a file of 4 MiB at that cluster size; small enough that
 * every array in `struct bootslot` has a fixed size. */
#define BOOTSLOT_FAT_MAX_SECTORS   1024
#define BOOTSLOT_MAX_CLUSTERS      131072
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
    uint32_t size;      /* bytes, from the directory entry */
    uint32_t chain_len; /* clusters in `chain` */
    uint32_t chain[BOOTSLOT_MAX_FILE_CLUSTERS];
};

/* Room for a menu id and its NUL: `candidate`, the longest id an install
   writes, takes ten bytes, and the block's keys are short. */
#define BOOTSLOT_ENTRY_MAX 24

/** The opened volume: checked geometry, one FAT, the four files. */
struct bootslot {
    struct bootslot_io io;
    uint32_t total_sectors;
    uint32_t cluster_sectors;
    uint32_t data_start;    /* the sector of cluster 2, partition-relative */
    uint32_t cluster_count; /* data clusters, numbered from 2 */
    uint32_t fat_start;
    uint32_t fat_sectors; /* the sectors of one FAT */
    /* The first FAT, which `bootslot_open` has compared with the second. */
    uint8_t fat[BOOTSLOT_FAT_MAX_SECTORS * BOOTSLOT_SECTOR];
    struct bootslot_file kernel;   /* /boot/kernel, the candidate slot */
    struct bootslot_file fallback; /* /boot/fallback */
    struct bootslot_file grubenv;  /* /boot/grub/grubenv */
    struct bootslot_file menu;     /* /boot/boot.cfg, when `have_menu` */
    int have_menu;                 /* 0 for a volume with no /boot/boot.cfg */
    /* The walk's bitmap, a bit for each cluster. */
    uint8_t visited[BOOTSLOT_MAX_CLUSTERS / 8];
};

/*
 * Every entry point returns 0 on success or -1 with one lowercase diagnostic
 * in `err` (always NUL-terminated, `errlen` its capacity) naming the cause.
 * The library keeps its sector buffers in static storage, so one call runs
 * at a time.
 */

/** Read and check the volume into `bs`, which keeps a copy of `io`: the BPB,
 *  both FATs (compared whole), and the namespace walk that resolves the four
 *  files and requires every reachable chain legal, loop-free and disjoint.
 *  Fails for a volume without /boot/kernel, /boot/fallback or a 1024-byte
 *  /boot/grub/grubenv, and for a /boot/boot.cfg that is empty or not a whole
 *  number of sectors. */
int bootslot_open(struct bootslot *bs, const struct bootslot_io *io, char *err, size_t errlen);

/**
 * Check the headers of a kernel a Multiboot loader can load, without changing
 * the image: an ELF32 i386 kernel, whose program headers the loader follows,
 * or an ELF64 x86-64 kernel, whose Multiboot header gives the load's
 * addresses through the a.out kludge.
 *
 * `image` holds the first `present` bytes of a file of `size` bytes. The ELF
 * header, an i386 kernel's program headers and both Multiboot headers must
 * fit `present`, and the Multiboot headers must lie in the first 8192 bytes.
 * The Multiboot header must be four-byte aligned with a valid checksum and
 * the flags ALIGN|MEMINFO, with AOUT_KLUDGE as well for an x86-64 kernel.
 * The multiboot2 header must be eight-byte aligned with a valid checksum.
 * Of its tags, an address tag and an entry tag come together and give the
 * load by address, which an x86-64 kernel's must do; a console or
 * framebuffer tag changes only the display; any other tag must be optional.
 * Load file ranges must fit `size`, load memory ranges the 32-bit address
 * space at or above 1 MiB, and the entry point must lie in a load's
 * file-backed bytes; an i386 load's physical address must equal its virtual
 * one.
 *
 * When `present` covers the whole image the disk-policy banner is judged as
 * well: the image must have the read-write banner and not the read-only one.
 * The read-only build issues no write command to a disk, so a machine booted
 * from it cannot mount its disk root and stops with nothing mounted; that
 * costs a boot and a trip to the machine, and the check catches it here
 * instead. A caller that passes a prefix is checking a slot it has already
 * installed, and the banner may lie past what it read, so the policy is not
 * judged there.
 */
int bootslot_validate_kernel(const uint8_t *image, uint32_t present, uint32_t size, char *err,
                             size_t errlen);

/**
 * Install the kernel `image` of `size` bytes into the candidate slot for one
 * trial boot, in the interruption-matrix order: check the image; read and
 * check the fallback file's headers; write `saved_entry=fallback` and drop
 * any `next_entry` in one write of the block; write the slot sector by
 * sector, zeros to its end; read the whole slot back and compare byte for
 * byte; then write `next_entry=candidate`.
 *
 * Fails with nothing written for an image that does not check or does not
 * fit the slot, a slot that is not a whole number of sectors, or a fallback
 * that is not a bootable kernel.
 */
int bootslot_install(struct bootslot *bs, const uint8_t *image, uint32_t size, char *err,
                     size_t errlen);

/**
 * Write `saved_entry=candidate` when the block has an empty `next_entry` and
 * no nonempty one.
 *
 * GRUB saves this marker before loading the trial kernel; it does not prove
 * a completed boot. The caller must check `rach2.slot=candidate`. Fails for
 * a nonempty `next_entry`, a trial not yet booted, and for a block with no
 * empty one.
 */
int bootslot_commit(struct bootslot *bs, char *err, size_t errlen);

/**
 * Copy the candidate slot into the fallback file and read the copy back.
 *
 * Requires `saved_entry=candidate`, no nonempty `next_entry`, both files a
 * whole number of sectors, a fallback file at least as long as the candidate
 * slot, and a candidate whose headers check. These environment values do
 * not establish which kernel ran, so boot the copied fallback once before
 * relying on it.
 *
 * The fallback's first sector is zeroed first, the remaining sectors are
 * copied and compared, and only then is the first sector restored and
 * compared. A failure before that final write leaves no valid ELF header; a
 * failure during it may leave a partial header.
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
 * next, so a slot that does not take the write fails here and not at the
 * next boot. The padding is '#' rather than zeros because GRUB reads the
 * whole file, and to GRUB a tail of '#' is a comment.
 *
 * An interrupted write can leave a new prefix above an old tail. The stub
 * defines the required entries before sourcing this file. The file is
 * unrestricted GRUB script and can change the variables used to select the
 * default or boot a kernel directly.
 *
 * Fails with nothing written for text larger than the slot and for a volume
 * that has no slot at all.
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
 * library. Fails for a block without GRUB's header line and for a line that
 * does not fit the block.
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

/** Read the environment block, BOOTSLOT_ENV_BYTES bytes, into `out`. */
int bootslot_read_env(struct bootslot *bs, uint8_t *out, char *err, size_t errlen);

#endif /* BOOTSLOT_H */
