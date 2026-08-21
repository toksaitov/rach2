#ifndef GRUBSLOT_H
#define GRUBSLOT_H

#include "bootslot.h"

/*
 * grubslot: where GRUB's core image lives in the post-MBR gap, and how one is
 * replaced without ever writing sector 0.
 *
 * Sector 0 has the partition table at byte 446, and a disk writes whole
 * sectors, so the eight bytes of boot code that point at the core image cannot
 * be touched without rewriting the table beside them. So they are never
 * touched: `boot.img[0x5c]` says LBA 1 for the life of the disk, and LBA 1 is
 * GRUB's `diskboot` sector, which has at offset 0x1f4 the blocklist naming
 * where the rest of the image lives and how long it is.
 *
 * That makes LBA 1 a selector. The gap has two tails, and replacing the core
 * image means writing the one that is not in use, verifying it, and then
 * rewriting LBA 1 to point at it. Everything before that last sector leaves the
 * old image selected and the machine booting as it did.
 *
 *     LBA 0            the MBR: boot code, then the partition table
 *     LBA 1            the entry sector: diskboot, and the blocklist
 *     LBA 2..1024      tail A, 1023 sectors
 *     LBA 1025..2047   tail B, 1023 sectors
 *
 * A complete core image is its entry sector plus its tail, so the largest one
 * that fits is 1024 sectors, of which 1023 go in a tail.
 */

#define GRUBSLOT_TAIL_A_LBA 2u
#define GRUBSLOT_TAIL_B_LBA 1025u
#define GRUBSLOT_TAIL_SECTORS 1023u
#define GRUBSLOT_MAX_IMAGE_SECTORS 1024u

/* Where the blocklist lives in the entry sector: an 8-byte LBA at 0x1f4, a
 * 16-bit sector count at 0x1fc, and the real-mode segment the tail is read
 * into at 0x1fe. The first two are the offsets `tools/grub_embed.py` patches
 * when the disk is built. The segment is not patched by anything here; it is
 * checked, because GRUB's own image builder always writes 0x0820 (the kernel
 * segment 0x0800 plus the entry sector) and a file that does not carry it is
 * not a core image for this platform. */
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
 * this code does not read, and cannot say which parts of the gap are free. */
#define GRUBSLOT_BLOCKLIST_NEXT_LEN_OFF 0x1f0

/**
 * Which tail the entry sector points at.
 *
 * Reads the blocklist out of `entry` and answers with the first LBA of the tail
 * it starts in. There must be exactly one record: a loader spread over more of
 * them occupies parts of the gap this code never reads. The count is judged
 * too, and must fit inside one slot: a core
 * image long enough to run out of tail A and into tail B has no inactive tail
 * to install into, and treating B as free would overwrite the running image.
 *
 * What this establishes is the layout, not who built the disk. A conventional
 * GRUB installation, whose core image also begins at LBA 2 in one extent,
 * passes; installing over one writes the half of the gap it is not running from
 * and selects it, leaving the old image where it is. So the tool claims LBA 2
 * through 2047 on whatever disk it runs on. On a disk this project built that
 * region is the tool's; on one it did not build, the region may hold something
 * no partition table mentions, and the install overwrites it. What the kernel
 * guarantees is narrower: `/dev/grubcore` is published only where no partition
 * overlaps LBA 1 through 2047 (`mbr_gap_clear`), so a disk whose first
 * partition starts at LBA 63 has no window to write through at all.
 *
 * @param entry  the 512 bytes at LBA 1.
 * @param out    the active tail's first LBA, untouched on failure.
 * @return 0, or -1 when the blocklist names neither tail.
 */
int grubslot_active(const uint8_t *entry, uint32_t *out);

/**
 * The tail a replacement should be written into: the one not in use.
 *
 * @param active a tail's first LBA, as `grubslot_active` reports it.
 * @return the other tail's first LBA, or 0 if `active` is neither.
 */
uint32_t grubslot_other(uint32_t active);

/**
 * Patch a complete core image's entry sector to load its tail from `tail_lba`.
 *
 * Writes the blocklist the image will be read through: the tail's first LBA and
 * the number of sectors after the entry sector. The image is judged first, so a
 * caller that gets a refusal here has written nothing. Judged means: it fits a
 * slot, it is read through one extent and not several, it has GRUB's
 * blocklist segment, and the tail length it declares for itself matches its
 * length in sectors.
 *
 * That last check catches less than it appears to. A core image declares a
 * count of sectors, not a count of bytes, and
 * no field in it records the byte length grub-mkimage gave it. So a transfer
 * cut short by enough to lose a whole sector is refused, and one cut inside
 * the final sector cannot be told from the whole file. Nor does any of this
 * establish that the image has the modules this disk needs; the shape is
 * checked, not the ability to boot. Check the transfer where it can be
 * checked, with `sha256sum` at both ends, before installing.
 *
 * @param image    the whole image, at least one sector.
 * @param sectors  its length in sectors, 2 through 1024.
 * @param tail_lba the tail to point at.
 * @return 0, or -1 with a diagnostic in `err`.
 */
int grubslot_patch(uint8_t *image, uint32_t sectors, uint32_t tail_lba, char *err, size_t errlen);

/**
 * Read or write one 512-byte sector of the gap.
 *
 * `write` is 1 to write `buf` to `lba` and 0 to read `lba` into it. A return of
 * 0 is success and anything else is a failure `grubslot_install` reports.
 */
/**
 * Round a core image file up to whole sectors and zero the padding.
 *
 * A core image is not a whole number of sectors and the gap is written in
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
 * shorter image leaves nothing of a longer predecessor behind.
 *
 * @param ctx        passed to `io` unchanged.
 * @param io         the sector reader and writer.
 * @param image      the whole core image, patched in place; `sectors` long.
 * @param sectors    its length in sectors, 2 through 1024.
 * @param scratch    one sector of workspace the caller owns.
 * @param out_target the tail written, set only on success.
 * @return 0, or -1 with a diagnostic in `err`.
 */
int grubslot_install(void *ctx, grubslot_io_fn io, uint8_t *image, uint32_t sectors,
                     uint8_t *scratch, uint32_t *out_target, char *err, size_t errlen);

#endif /* GRUBSLOT_H */
