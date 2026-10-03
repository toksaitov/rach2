/**
 * Validate and replace GRUB core images using caller-supplied sector I/O.
 *
 * The layout of the boot loader window and the contract of each function
 * are in `grubslot.h`.
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

/** Copy `what` into `err`, cut to fit and terminated, and return -1. */
static int say(char *err, size_t errlen, const char *what)
{
    if (err && errlen) {
        strncpy(err, what, errlen - 1);
        err[errlen - 1] = '\0';
    }
    return -1;
}

int grubslot_window(uint64_t first, uint64_t sectors, struct grubslot_window *w, char *err,
                    size_t errlen)
{
    uint32_t tail;

    if (first == 0u) {
        return say(err, errlen, "the boot loader window includes sector 0");
    }
    if (sectors < 3u) {
        return say(err, errlen, "the boot loader window has no room for two tails");
    }
    if (first > 0xffffffffu || sectors > 0x100000000ull - first) {
        return say(err, errlen, "the boot loader window reaches past 2^32 sectors");
    }
    tail = (uint32_t)((sectors - 1u) / 2u);
    if (tail > GRUBSLOT_TAIL_SECTORS) {
        tail = GRUBSLOT_TAIL_SECTORS;
    }
    w->entry = (uint32_t)first;
    w->tail_a = w->entry + 1u;
    w->tail_b = w->tail_a + tail;
    w->tail = tail;
    return 0;
}

int grubslot_active(const struct grubslot_window *w, const uint8_t *entry, uint32_t *out)
{
    uint64_t lba = le64(entry + GRUBSLOT_BLOCKLIST_LBA_OFF);
    uint16_t len = le16(entry + GRUBSLOT_BLOCKLIST_LEN_OFF);

    /* Zero names no image; more than a tail means the image spans both. */
    if (len == 0u || (uint32_t)len > w->tail) {
        return -1;
    }
    /* A second record: the loader lies in extents this code does not read. */
    if (le16(entry + GRUBSLOT_BLOCKLIST_NEXT_LEN_OFF) != 0u) {
        return -1;
    }
    if (lba == (uint64_t)w->tail_a) {
        *out = w->tail_a;
        return 0;
    }
    if (lba == (uint64_t)w->tail_b) {
        *out = w->tail_b;
        return 0;
    }
    return -1;
}

uint32_t grubslot_other(const struct grubslot_window *w, uint32_t active)
{
    if (active == w->tail_a) {
        return w->tail_b;
    }
    if (active == w->tail_b) {
        return w->tail_a;
    }
    return 0;
}

int grubslot_patch(const struct grubslot_window *w, uint8_t *image, uint32_t sectors,
                   uint32_t tail_lba, char *err, size_t errlen)
{
    if (sectors < 2u) {
        return say(err, errlen, "a core image is at least two sectors");
    }
    if (sectors - 1u > w->tail) {
        return say(err, errlen, "the core image does not fit a tail slot");
    }
    if (tail_lba != w->tail_a && tail_lba != w->tail_b) {
        return say(err, errlen, "that is not a tail slot");
    }
    if (le16(image + GRUBSLOT_BLOCKLIST_SEG_OFF) != GRUBSLOT_BLOCKLIST_SEG) {
        return say(err, errlen, "that file is not a GRUB core image for this platform");
    }
    /* GRUB reads records to a zero count; a second names unwritten sectors. */
    if (le16(image + GRUBSLOT_BLOCKLIST_NEXT_LEN_OFF) != 0u) {
        return say(err, errlen, "that core image is read through more than one extent");
    }
    /* A tail length unlike the file's means a truncated or foreign file. */
    if ((uint32_t)le16(image + GRUBSLOT_BLOCKLIST_LEN_OFF) != sectors - 1u) {
        return say(err, errlen,
                   "that core image is not the length it declares; it is truncated or "
                   "is not a core image");
    }

    put_le64(image + GRUBSLOT_BLOCKLIST_LBA_OFF, (uint64_t)tail_lba);
    put_le16(image + GRUBSLOT_BLOCKLIST_LEN_OFF, (uint16_t)(sectors - 1u));
    return 0;
}

int grubslot_prepare(uint8_t *image, uint32_t capacity, uint32_t bytes, uint32_t *sectors,
                     char *err, size_t errlen)
{
    uint32_t n;

    if (bytes == 0u) {
        return say(err, errlen, "that file is empty");
    }
    /* Bound the bytes before rounding: a rounded count near the top wraps. */
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

int grubslot_install(const struct grubslot_window *w, void *ctx, grubslot_io_fn io, uint8_t *image,
                     uint32_t sectors, uint8_t *scratch, uint32_t *out_target, char *err,
                     size_t errlen)
{
    uint32_t active, target, n, i;

    if (io(ctx, w->entry, scratch, 0) != 0) {
        return say(err, errlen, "the entry sector did not read");
    }
    if (grubslot_active(w, scratch, &active) != 0) {
        return say(err, errlen,
                   "the entry sector's blocklist names neither tail slot, or is "
                   "longer than one; this disk does not have the layout an "
                   "install needs");
    }
    target = grubslot_other(w, active);
    if (grubslot_patch(w, image, sectors, target, err, errlen) != 0) {
        return -1;
    }

    for (n = 0; n < w->tail; n++) {
        if (n + 1u < sectors) {
            memcpy(scratch, image + (n + 1u) * BOOTSLOT_SECTOR, BOOTSLOT_SECTOR);
        } else {
            memset(scratch, 0, BOOTSLOT_SECTOR);
        }
        if (io(ctx, target + n, scratch, 1) != 0) {
            return say(err, errlen, "the tail slot did not write");
        }
    }
    for (n = 0; n < w->tail; n++) {
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

    if (io(ctx, w->entry, image, 1) != 0) {
        return say(err, errlen, "the entry sector did not write; this disk may not boot");
    }
    if (io(ctx, w->entry, scratch, 0) != 0 || memcmp(scratch, image, BOOTSLOT_SECTOR) != 0) {
        return say(err, errlen, "the entry sector did not read back; this disk may not boot");
    }
    *out_target = target;
    return 0;
}
