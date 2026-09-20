/**
 * Validate and replace GRUB core images using caller-supplied sector I/O.
 */

#include <string.h>

#include "grubslot.h"

static uint64_t le64(const uint8_t *p)
{
    uint64_t v = 0;
    int i;

    for (i = 7; i >= 0; i--) {
        v = (v << 8) | (uint64_t)p[i];
    }
    return v;
}

static void put_le64(uint8_t *p, uint64_t v)
{
    int i;

    for (i = 0; i < 8; i++) {
        p[i] = (uint8_t)(v & 0xffu);
        v >>= 8;
    }
}

static uint16_t le16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static void put_le16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xffu);
    p[1] = (uint8_t)((v >> 8) & 0xffu);
}

int grubslot_active(const uint8_t *entry, uint32_t *out)
{
    uint64_t lba = le64(entry + GRUBSLOT_BLOCKLIST_LBA_OFF);
    uint16_t len = le16(entry + GRUBSLOT_BLOCKLIST_LEN_OFF);

    /* A count of zero names no image, and one past a slot's end means the
       running image occupies both tails, so there is nowhere to install. */
    if (len == 0u || (uint32_t)len > GRUBSLOT_TAIL_SECTORS) {
        return -1;
    }
    /* And there must be no second record: a loader read through several extents
       occupies parts of the gap this code does not read. */
    if (le16(entry + GRUBSLOT_BLOCKLIST_NEXT_LEN_OFF) != 0u) {
        return -1;
    }
    if (lba == (uint64_t)GRUBSLOT_TAIL_A_LBA) {
        *out = GRUBSLOT_TAIL_A_LBA;
        return 0;
    }
    if (lba == (uint64_t)GRUBSLOT_TAIL_B_LBA) {
        *out = GRUBSLOT_TAIL_B_LBA;
        return 0;
    }
    return -1;
}

uint32_t grubslot_other(uint32_t active)
{
    if (active == GRUBSLOT_TAIL_A_LBA) {
        return GRUBSLOT_TAIL_B_LBA;
    }
    if (active == GRUBSLOT_TAIL_B_LBA) {
        return GRUBSLOT_TAIL_A_LBA;
    }
    return 0;
}

int grubslot_patch(uint8_t *image, uint32_t sectors, uint32_t tail_lba, char *err, size_t errlen)
{
    if (sectors < 2u) {
        if (err && errlen) {
            strncpy(err, "a core image is at least two sectors", errlen - 1);
            err[errlen - 1] = '\0';
        }
        return -1;
    }
    if (sectors > GRUBSLOT_MAX_IMAGE_SECTORS) {
        if (err && errlen) {
            strncpy(err, "the core image does not fit a tail slot", errlen - 1);
            err[errlen - 1] = '\0';
        }
        return -1;
    }
    if (tail_lba != GRUBSLOT_TAIL_A_LBA && tail_lba != GRUBSLOT_TAIL_B_LBA) {
        if (err && errlen) {
            strncpy(err, "that is not a tail slot", errlen - 1);
            err[errlen - 1] = '\0';
        }
        return -1;
    }
    if (le16(image + GRUBSLOT_BLOCKLIST_SEG_OFF) != GRUBSLOT_BLOCKLIST_SEG) {
        if (err && errlen) {
            strncpy(err, "that file is not a GRUB core image for this platform", errlen - 1);
            err[errlen - 1] = '\0';
        }
        return -1;
    }
    /* The candidate is held to the rule the running image is held to: one
       record. GRUB reads the list downward until a count of zero ends it, so a
       second record in the file being installed would send the next boot to an
       extent nothing here wrote. */
    if (le16(image + GRUBSLOT_BLOCKLIST_NEXT_LEN_OFF) != 0u) {
        if (err && errlen) {
            strncpy(err, "that core image is read through more than one extent", errlen - 1);
            err[errlen - 1] = '\0';
        }
        return -1;
    }
    /* A core image declares its own tail length, and grub-mkimage writes the
       whole of it. A file whose declaration disagrees with its size arrived
       truncated or is not the file it claims to be, and installing it would
       leave a machine that cannot boot and cannot be repaired remotely. */
    if ((uint32_t)le16(image + GRUBSLOT_BLOCKLIST_LEN_OFF) != sectors - 1u) {
        if (err && errlen) {
            strncpy(err,
                    "that core image is not the length it declares; it is truncated or "
                    "is not a core image",
                    errlen - 1);
            err[errlen - 1] = '\0';
        }
        return -1;
    }

    put_le64(image + GRUBSLOT_BLOCKLIST_LBA_OFF, (uint64_t)tail_lba);
    put_le16(image + GRUBSLOT_BLOCKLIST_LEN_OFF, (uint16_t)(sectors - 1u));
    return 0;
}

static int say(char *err, size_t errlen, const char *what)
{
    if (err && errlen) {
        strncpy(err, what, errlen - 1);
        err[errlen - 1] = '\0';
    }
    return -1;
}

int grubslot_prepare(uint8_t *image, uint32_t capacity, uint32_t bytes, uint32_t *sectors,
                     char *err, size_t errlen)
{
    uint32_t n;

    if (bytes == 0u) {
        return say(err, errlen, "that file is empty");
    }
    /* Judge the byte count before rounding it, not after: rounding up a length
       near the end of the range wraps, and a wrapped count passes every bound
       there is. */
    if (bytes > capacity || bytes > GRUBSLOT_MAX_IMAGE_SECTORS * (uint32_t)BOOTSLOT_SECTOR) {
        return say(err, errlen, "the core image does not fit a tail slot");
    }
    n = 1u + (bytes - 1u) / BOOTSLOT_SECTOR;
    if (n * BOOTSLOT_SECTOR > capacity) {
        return say(err, errlen, "the core image does not fit a tail slot");
    }
    memset(image + bytes, 0, n * BOOTSLOT_SECTOR - bytes);
    *sectors = n;
    return 0;
}

int grubslot_install(void *ctx, grubslot_io_fn io, uint8_t *image, uint32_t sectors,
                     uint8_t *scratch, uint32_t *out_target, char *err, size_t errlen)
{
    uint32_t active, target, n, i;

    if (io(ctx, 1u, scratch, 0) != 0) {
        return say(err, errlen, "the entry sector did not read");
    }
    if (grubslot_active(scratch, &active) != 0) {
        return say(err, errlen,
                   "the entry sector's blocklist names neither tail slot, or is "
                   "longer than one; this disk does not have the layout an "
                   "install needs");
    }
    target = grubslot_other(active);
    if (grubslot_patch(image, sectors, target, err, errlen) != 0) {
        return -1;
    }

    for (n = 0; n < GRUBSLOT_TAIL_SECTORS; n++) {
        if (n + 1u < sectors) {
            memcpy(scratch, image + (n + 1u) * BOOTSLOT_SECTOR, BOOTSLOT_SECTOR);
        } else {
            memset(scratch, 0, BOOTSLOT_SECTOR);
        }
        if (io(ctx, target + n, scratch, 1) != 0) {
            return say(err, errlen, "the tail slot did not write");
        }
    }
    for (n = 0; n < GRUBSLOT_TAIL_SECTORS; n++) {
        if (io(ctx, target + n, scratch, 0) != 0) {
            return say(err, errlen, "the tail slot did not read back");
        }
        if (n + 1u < sectors) {
            if (memcmp(scratch, image + (n + 1u) * BOOTSLOT_SECTOR, BOOTSLOT_SECTOR) != 0) {
                return say(err, errlen,
                           "the tail slot did not read back what was written; "
                           "the running boot loader is untouched");
            }
        } else {
            for (i = 0; i < BOOTSLOT_SECTOR; i++) {
                if (scratch[i] != 0) {
                    return say(err, errlen,
                               "the tail slot's fill did not read back; "
                               "the running boot loader is untouched");
                }
            }
        }
    }

    if (io(ctx, 1u, image, 1) != 0) {
        return say(err, errlen, "the entry sector did not write; this disk may not boot");
    }
    if (io(ctx, 1u, scratch, 0) != 0 || memcmp(scratch, image, BOOTSLOT_SECTOR) != 0) {
        return say(err, errlen, "the entry sector did not read back; this disk may not boot");
    }
    *out_target = target;
    return 0;
}
