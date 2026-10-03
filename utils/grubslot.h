#ifndef GRUBSLOT_H
#define GRUBSLOT_H

#include "bootslot.h"

/*
 * grubslot: where GRUB's BIOS core image lives, and how one is replaced
 * without ever writing sector 0.
 *
 * GRUB's boot code in sector 0 loads one sector, the one whose LBA is at
 * `boot.img[0x5c]`, and that sector, GRUB's `diskboot`, has at offset 0x1f4
 * the blocklist naming where the rest of the image lives and how long it is.
 * Sector 0 has the partition table at byte 446, the protective one on a GPT
 * disk, and a disk writes whole sectors, so the eight bytes that point at the
 * core cannot be touched without rewriting the table beside them. So they are
 * never touched: they name the window's first sector for the life of the disk.
 *
 * That makes the first sector a selector. The rest of the window has two
 * tails, and replacing the core image means writing the one that is not in
 * use, verifying it, and then rewriting the entry sector to point at it.
 * Everything before that last sector leaves the old image selected and the
 * machine booting as it did.
 *
 * The window is the range the kernel publishes as `/dev/grubcore`. On an MBR
 * disk it is the post-MBR gap; on a GPT disk, whose sectors after the MBR are
 * the table's, it is the BIOS boot partition, LBA 34 through 2047 on the disks
 * this project builds:
 *
 *     MBR disk                          GPT disk
 *     LBA 1           entry sector      LBA 34          entry sector
 *     LBA 2..1024     tail A, 1023      LBA 35..1040    tail A, 1006
 *     LBA 1025..2047  tail B, 1023      LBA 1041..2046  tail B, 1006
 *                                       LBA 2047        not used
 *
 * In general a window of N sectors from LBA W has its entry sector at W and
 * two tails of T = min((N - 1) / 2, 1023) sectors, at W + 1 and W + 1 + T.
 * A complete core image is its entry sector plus its tail, so the largest one
 * that fits is T + 1 sectors, and never more than 1024. The blocklist's LBA is
 * absolute on the disk, as GRUB reads it.
 */

/* The longest tail, and so the longest image, any window has. A tail is
 * written whole on every install, so this also bounds what one costs. */
#define GRUBSLOT_TAIL_SECTORS      1023u
#define GRUBSLOT_MAX_IMAGE_SECTORS 1024u

/* Where the blocklist lives in the entry sector: an 8-byte LBA at 0x1f4, a
 * 16-bit sector count at 0x1fc, and the real-mode segment the tail is read
 * into at 0x1fe. The first two are the offsets `grubembed` patches when the
 * disk is built. The segment is not patched by anything here; it is checked,
 * because GRUB's own image builder always writes 0x0820 (the kernel segment
 * 0x0800 plus the entry sector) and a file that does not carry it is not a
 * core image for this platform. */
#define GRUBSLOT_BLOCKLIST_LBA_OFF 0x1f4
#define GRUBSLOT_BLOCKLIST_LEN_OFF 0x1fc
#define GRUBSLOT_BLOCKLIST_SEG_OFF 0x1fe
#define GRUBSLOT_BLOCKLIST_SEG     0x0820

/* GRUB's boot code reads a LIST of these records, 12 bytes each (an 8-byte LBA,
 * a 2-byte count, a 2-byte segment), growing downward from 0x1f4 and ending at
 * the first record whose count is zero. The record below the first therefore
 * begins at 0x1e8 and its count lives at 0x1f0. Every image this project builds
 * has exactly one record, and so does every image grub-mkimage writes for a
 * contiguous embedding; a second one means the loader is spread over extents
 * this code does not read, and cannot say which parts of the window are
 * free. */
#define GRUBSLOT_BLOCKLIST_NEXT_LEN_OFF 0x1f0

/** The shape of one window: its entry sector and its two tails. */
struct grubslot_window {
    uint32_t entry;  /* the window's first sector, which boot.img loads */
    uint32_t tail_a; /* entry + 1 */
    uint32_t tail_b; /* tail_a + tail */
    uint32_t tail;   /* sectors in each tail */
};

/**
 * Lay out the window of `sectors` sectors that begins at LBA `first`.
 *
 * `first` and `sectors` are what the kernel reports for `/dev/grubcore`
 * (`HDIO_GETGEO` and `BLKGETSIZE64`), never what an entry sector says. The
 * window must leave out sector 0, end below 2^32 sectors, and have room for an
 * entry sector and two tails of at least one sector each.
 *
 * @return 0 with `w` filled, or -1 with a diagnostic in `err` and `w`
 *         untouched.
 */
int grubslot_window(uint64_t first, uint64_t sectors, struct grubslot_window *w, char *err,
                    size_t errlen);

/**
 * Which tail the entry sector points at.
 *
 * Reads the blocklist out of `entry` and answers with the first LBA of the tail
 * it starts in. There must be exactly one record: a loader spread over more of
 * them occupies parts of the window this code never reads. The count is judged
 * too, and must fit inside one tail: a core image long enough to run out of
 * tail A and into tail B has no inactive tail to install into, and treating B
 * as free would overwrite the running image.
 *
 * What this establishes is the layout, not who built the disk. A conventional
 * GRUB installation, whose core image also begins one sector into the window in
 * one extent, passes; installing over one writes the half of the window it is
 * not running from and selects it, leaving the old image where it is. So the
 * tool claims the whole window on whatever disk it runs on. What the kernel
 * guarantees is that the window is no more than that: on an MBR disk
 * `/dev/grubcore` is published only where no partition overlaps LBA 1 through
 * 2047 (`mbr_gap_clear`), and on a GPT disk only over the one BIOS boot
 * partition (`part_bios_boot`).
 *
 * @param w      the window, as `grubslot_window` laid it out.
 * @param entry  the 512 bytes at `w->entry`.
 * @param out    the active tail's first LBA, untouched on failure.
 * @return 0, or -1 when the blocklist names neither tail.
 */
int grubslot_active(const struct grubslot_window *w, const uint8_t *entry, uint32_t *out);

/**
 * The tail a replacement should be written into: the one not in use.
 *
 * @param active a tail's first LBA, as `grubslot_active` reports it.
 * @return the other tail's first LBA, or 0 if `active` is neither.
 */
uint32_t grubslot_other(const struct grubslot_window *w, uint32_t active);

/**
 * Patch a complete core image's entry sector to load its tail from `tail_lba`.
 *
 * Writes the blocklist the image will be read through: the tail's first LBA and
 * the number of sectors after the entry sector. The image is judged first, so a
 * caller that gets a refusal here has written nothing. Judged means: it fits a
 * tail of this window, it is read through one extent and not several, it has
 * GRUB's blocklist segment, and the tail length it declares for itself matches
 * its length in sectors.
 *
 * That last check catches less than it appears to. A core image declares a
 * count of sectors, not a count of bytes, and no field in it records the byte
 * length grub-mkimage gave it. So a transfer cut short by enough to lose a
 * whole sector is refused, and one cut inside the final sector cannot be told
 * from the whole file. Nor does any of this establish that the image has the
 * modules this disk needs (`part_gpt` for a GPT disk, `part_msdos` for an MBR
 * one); the shape is checked, not the ability to boot. Check the transfer where
 * it can be checked, with `sha256sum` at both ends, before installing.
 *
 * @param image    the whole image, at least one sector.
 * @param sectors  its length in sectors, 2 through `w->tail + 1`.
 * @param tail_lba the tail to point at.
 * @return 0, or -1 with a diagnostic in `err`.
 */
int grubslot_patch(const struct grubslot_window *w, uint8_t *image, uint32_t sectors,
                   uint32_t tail_lba, char *err, size_t errlen);

/**
 * Round a core image file up to whole sectors and zero the padding.
 *
 * A core image is not a whole number of sectors and the window is written in
 * sectors, so the bytes past the file's end are zeroed, not left holding
 * whatever the buffer had. This is also where the length check in
 * `grubslot_patch` loses its precision: a file cut short inside its last sector
 * rounds to the same sector count as the whole one.
 *
 * @param image    the buffer holding the file, written past `bytes`.
 * @param capacity the buffer's size.
 * @param bytes    the file's length.
 * @param sectors  the rounded length, set only on success.
 * @return 0, or -1 with a diagnostic in `err`.
 */
int grubslot_prepare(uint8_t *image, uint32_t capacity, uint32_t bytes, uint32_t *sectors,
                     char *err, size_t errlen);

/**
 * Read or write one 512-byte sector of the window.
 *
 * `lba` is absolute on the disk. `write` is 1 to write `buf` to `lba` and 0 to
 * read `lba` into it. A return of 0 is success and anything else is a failure
 * `grubslot_install` reports.
 */
typedef int (*grubslot_io_fn)(void *ctx, uint32_t lba, uint8_t *buf, int write);

/**
 * Replace the core image, leaving the machine bootable at every point but one.
 *
 * Reads the entry sector to learn which tail is in use, patches `image` to load
 * from the other one, writes the whole of that tail, reads all of it back and
 * compares, and only then writes the entry sector. Every failure before that
 * last write leaves the old entry sector and the old tail selected, so the
 * machine boots as it did. The last write is the one risk in the scheme: a torn
 * write to the entry sector makes GRUB unreachable, and recovery cannot help,
 * because GRUB is what loads recovery.
 *
 * The whole tail is written, not only the sectors the image reaches, so a
 * shorter image leaves nothing of a longer predecessor behind. No sector
 * outside `w->entry` and the two tails is read or written.
 *
 * @param w          the window, as `grubslot_window` laid it out.
 * @param ctx        passed to `io` unchanged.
 * @param io         the sector reader and writer.
 * @param image      the whole core image, patched in place; `sectors` long.
 * @param sectors    its length in sectors, 2 through `w->tail + 1`.
 * @param scratch    one sector of workspace the caller owns.
 * @param out_target the tail written, set only on success.
 * @return 0, or -1 with a diagnostic in `err`.
 */
int grubslot_install(const struct grubslot_window *w, void *ctx, grubslot_io_fn io, uint8_t *image,
                     uint32_t sectors, uint8_t *scratch, uint32_t *out_target, char *err,
                     size_t errlen);

#endif /* GRUBSLOT_H */
