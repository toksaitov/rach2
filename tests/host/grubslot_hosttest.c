/*
 * grubslot_hosttest.c: the two core-image tails of a boot loader window that
 * utils/grubslot.c lays out, and which one is in use.
 *
 * host-deps: utils/grubslot.c
 *
 * Sector 0 has the partition table, and a disk writes whole sectors, so the
 * boot code's pointer at byte 0x5c cannot be rewritten without rewriting the
 * table beside it. grubslot.c leaves sector 0 alone: the window's first
 * sector is a selector, the rest has two tails, and a replacement goes into
 * the one not in use. The window is the post-MBR gap of an MBR disk, LBA 1
 * through 2047, or the BIOS boot partition of a GPT disk, LBA 34 through
 * 2047 on the disks this project builds. A wrong tail boundary or blocklist
 * offset makes a disk that does not boot and cannot be repaired remotely, so
 * the rows check each number.
 */

extern int printf(const char *fmt, ...);

#include <string.h>

#include "../../utils/grubslot.h"

static int fails;

#define CHECK(cond, what)                   \
    do {                                    \
        if (!(cond)) {                      \
            fails++;                        \
            printf("  FAIL: %s\n", (what)); \
        }                                   \
    } while (0)

static unsigned char entry[512];
static unsigned char image[GRUBSLOT_MAX_IMAGE_SECTORS * 512];
static char err[128];

/** Write into the core image's first sector at `p` the fields grub-mkimage
 *  writes there: the tail's length, `sectors - 1`, the segment the tail is
 *  read into, and a zero count for the next blocklist record. */
static void stamp_core(unsigned char *p, unsigned long sectors)
{
    p[GRUBSLOT_BLOCKLIST_LEN_OFF] = (unsigned char)((sectors - 1u) & 0xffu);
    p[GRUBSLOT_BLOCKLIST_LEN_OFF + 1] = (unsigned char)(((sectors - 1u) >> 8) & 0xffu);
    p[GRUBSLOT_BLOCKLIST_SEG_OFF] = (unsigned char)(GRUBSLOT_BLOCKLIST_SEG & 0xffu);
    p[GRUBSLOT_BLOCKLIST_SEG_OFF + 1] = (unsigned char)((GRUBSLOT_BLOCKLIST_SEG >> 8) & 0xffu);
    p[GRUBSLOT_BLOCKLIST_NEXT_LEN_OFF] = 0;
    p[GRUBSLOT_BLOCKLIST_NEXT_LEN_OFF + 1] = 0;
}

/** Fill `p` with a core image `sectors` long, as grub-mkimage leaves one:
 *  bytes of 0x5c with the fields of `stamp_core`. */
static void make_core(unsigned char *p, unsigned long sectors)
{
    memset(p, 0x5c, sectors * 512u);
    stamp_core(p, sectors);
}

/** Store `lba` as the blocklist's 8-byte little-endian first sector in the
 *  entry sector at `p`. */
static void set_blocklist(unsigned char *p, unsigned long long lba)
{
    int i;

    for (i = 0; i < 8; i++) {
        p[GRUBSLOT_BLOCKLIST_LBA_OFF + i] = (unsigned char)(lba & 0xffu);
        lba >>= 8;
    }
}

/** The blocklist's first sector in the entry sector at `p`. */
static unsigned long long get_blocklist(const unsigned char *p)
{
    unsigned long long v = 0;
    int i;

    for (i = 7; i >= 0; i--) {
        v = (v << 8) | (unsigned long long)p[GRUBSLOT_BLOCKLIST_LBA_OFF + i];
    }
    return v;
}

/* A model of the disk's first 2048 sectors, whose io can report a write as
   done without doing it. A disk that loses a write silently is the failure
   the readback exists for, and a real device cannot be made to do it on
   request. Every sector outside the window under test starts as 0xee, and
   an access outside the window is counted, so a stray read or write is
   caught whether or not it changes a byte. */
static unsigned char gap[2048 * 512];
static long drops_from = -1; /* first sector whose writes drop; -1, none */
static int writes, strays;
static const struct grubslot_window *in_use;

/** The sector io `grubslot_install` calls: read or write sector `lba` of the
 *  model through `buf`. Counts writes, and accesses outside `in_use` in
 *  `strays`; drops a write at or past `drops_from` while reporting it done.
 *  Returns 0, or -1 for a sector past the model. */
static int model_io(void *ctx, uint32_t lba, uint8_t *buf, int write)
{
    (void)ctx;
    if (lba >= 2048u) {
        return -1;
    }
    if (lba < in_use->entry || lba >= in_use->tail_b + in_use->tail) {
        strays++;
    }
    if (write) {
        writes++;
        if (drops_from >= 0 && (long)lba >= drops_from) {
            return 0;
        }
        memcpy(gap + (size_t)lba * 512, buf, 512);
    } else {
        memcpy(buf, gap + (size_t)lba * 512, 512);
    }
    return 0;
}

/**
 * Make the model a disk before an install over the window `w`: an entry
 * sector naming `active`, an old image under it whose first sector is 0xa0,
 * and 0x77 in the rest of both tails, so a fill that was not written shows.
 * Clears the counters and the dropped writes. A window laid out wrong may
 * run past the model; it is cut at the model's end, and the rows that check
 * the layout report it.
 */
static void model_reset(const struct grubslot_window *w, unsigned long long active)
{
    size_t end = (size_t)w->tail_b + w->tail;

    if (end > 2048u) {
        end = 2048u;
    }
    memset(gap, 0xee, sizeof(gap));
    memset(gap + (size_t)w->entry * 512, 0x77, (end - w->entry) * 512u);
    memset(gap + (size_t)w->entry * 512, 0, 512);
    stamp_core(gap + (size_t)w->entry * 512, 2);
    set_blocklist(gap + (size_t)w->entry * 512, active);
    memset(gap + (size_t)active * 512, 0xa0, 512);
    in_use = w;
    drops_from = -1;
    writes = 0;
    strays = 0;
}

/** Whether every sector of the model outside `[from, to)` still has 0xee. */
static int outside_untouched(uint32_t from, uint32_t to)
{
    size_t i;

    for (i = 0; i < sizeof(gap); i++) {
        if ((i < (size_t)from * 512 || i >= (size_t)to * 512) && gap[i] != 0xee) {
            return 0;
        }
    }
    return 1;
}

/**
 * Check the windows' shapes, and lay out the gap in `mbr` and the BIOS boot
 * partition in `gpt` for the rows after.
 *
 * In the gap a tail is 1023 sectors, and the two are adjacent and disjoint,
 * so between them they cover LBA 2 through 2047 and nothing else: LBA 0 is
 * the MBR and LBA 1 the selector. The BIOS boot partition the disk builder
 * and the installer make, LBA 34 through 2047, is 2014 sectors: two tails
 * of 1006 at LBA 35 and 1041, and one sector left over, which nothing
 * writes. A partition of a mebibyte has tails of 1023, and a larger one is
 * capped there, since a tail is written whole on every install. Windows the
 * kernel does not publish are refused all the same: one over sector 0, one
 * too small for two tails, one that runs past the last 32-bit sector and
 * one that starts past it.
 */
static void shapes(struct grubslot_window *mbr, struct grubslot_window *gpt)
{
    struct grubslot_window w;

    CHECK(grubslot_window(1, 2047, mbr, err, sizeof(err)) == 0, "the post-MBR gap is a window");
    CHECK(mbr->entry == 1 && mbr->tail_a == 2, "tail A starts at LBA 2, just past the selector");
    CHECK(mbr->tail == GRUBSLOT_TAIL_SECTORS && mbr->tail_b == mbr->tail_a + mbr->tail,
          "tail B starts where tail A ends");
    CHECK(mbr->tail_b + mbr->tail == 2048, "and tail B ends where the first partition begins");
    CHECK(GRUBSLOT_MAX_IMAGE_SECTORS == GRUBSLOT_TAIL_SECTORS + 1,
          "a whole image is its entry sector plus a tail");

    CHECK(grubslot_window(34, 2014, gpt, err, sizeof(err)) == 0,
          "the BIOS boot partition is a window");
    CHECK(gpt->entry == 34 && gpt->tail_a == 35,
          "its entry sector is the partition's first, and tail A follows it");
    CHECK(gpt->tail == 1006 && gpt->tail_b == 1041, "two tails of 1006 sectors, B at LBA 1041");
    CHECK(gpt->tail_b + gpt->tail == 2047, "and B ends one sector before the partition does");

    CHECK(grubslot_window(2048, 2048, &w, err, sizeof(err)) == 0 && w.tail == 1023 &&
              w.tail_b == 2048 + 1 + 1023,
          "a 1 MiB partition has tails of 1023");
    CHECK(grubslot_window(2048, 1u << 20, &w, err, sizeof(err)) == 0 &&
              w.tail == GRUBSLOT_TAIL_SECTORS,
          "and a larger one is capped at the longest tail");

    w = *gpt;
    CHECK(grubslot_window(0, 2048, &w, err, sizeof(err)) == -1,
          "a window over sector 0 is refused");
    CHECK(strstr(err, "sector 0") != NULL, "and says so");
    CHECK(grubslot_window(34, 2, &w, err, sizeof(err)) == -1,
          "a window too small for two tails is refused");
    CHECK(grubslot_window(0xffffff00u, 0x100, &w, err, sizeof(err)) == 0,
          "a window ending at the last 32-bit sector is laid out");
    CHECK(grubslot_window(0xffffff00u, 0x101, &w, err, sizeof(err)) == -1,
          "and one sector further is refused");
    CHECK(grubslot_window(0x100000000ull, 2048, &w, err, sizeof(err)) == -1,
          "as is one that starts past 32 bits");
}

/**
 * Check which tail an entry sector names, in the window it is read in. The
 * same LBA is a tail in one window and nothing in the other.
 *
 * In the gap, a blocklist naming LBA 2 or 1025 names tail A or B, and the
 * other tail is the other one. One that does not begin at a tail boundary
 * (LBA 3, the MBR, the selector, or past the gap) names no slot. That is a
 * check on the layout and not on who built the disk: a conventional GRUB
 * installation, whose one extent also begins at LBA 2, passes it. In the
 * partition, the blocklist grubembed writes names LBA 35, one past the
 * partition's first sector, and an install moves it to 1041; the gap's
 * tails, the entry sector and the last sector of tail A name nothing there.
 * A running image of 1007 tail sectors fits the gap's tail and runs out of
 * the partition's tail A into its B, which leaves no inactive tail.
 */
static void selectors(const struct grubslot_window *mbr, const struct grubslot_window *gpt)
{
    uint32_t active;

    memset(entry, 0, sizeof(entry));
    stamp_core(entry, 2);
    set_blocklist(entry, mbr->tail_a);
    CHECK(grubslot_active(mbr, entry, &active) == 0 && active == mbr->tail_a,
          "a blocklist naming tail A reports tail A");
    CHECK(grubslot_other(mbr, active) == mbr->tail_b, "and the other tail is B");

    set_blocklist(entry, mbr->tail_b);
    CHECK(grubslot_active(mbr, entry, &active) == 0 && active == mbr->tail_b,
          "a blocklist naming tail B reports tail B");
    CHECK(grubslot_other(mbr, active) == mbr->tail_a, "and the other tail is A");

    set_blocklist(entry, 3);
    CHECK(grubslot_active(mbr, entry, &active) == -1, "a blocklist inside a tail names no slot");
    set_blocklist(entry, 0);
    CHECK(grubslot_active(mbr, entry, &active) == -1, "nor does one naming the MBR");
    set_blocklist(entry, 1);
    CHECK(grubslot_active(mbr, entry, &active) == -1, "nor one naming the selector itself");
    set_blocklist(entry, 2048);
    CHECK(grubslot_active(mbr, entry, &active) == -1, "nor one past the gap");
    CHECK(grubslot_other(mbr, 0) == 0, "and there is no other tail to a slot that is not one");

    set_blocklist(entry, 35);
    CHECK(grubslot_active(gpt, entry, &active) == 0 && active == 35,
          "in the BIOS boot partition, LBA 35 is tail A");
    CHECK(grubslot_other(gpt, active) == 1041, "and the other tail is at LBA 1041");
    set_blocklist(entry, 1041);
    CHECK(grubslot_active(gpt, entry, &active) == 0 && active == 1041 &&
              grubslot_other(gpt, active) == 35,
          "LBA 1041 is tail B, and the other is A");
    set_blocklist(entry, mbr->tail_a);
    CHECK(grubslot_active(gpt, entry, &active) == -1,
          "the gap's tail A is nothing in the partition, which the table has there");
    set_blocklist(entry, mbr->tail_b);
    CHECK(grubslot_active(gpt, entry, &active) == -1, "nor is the gap's tail B");
    set_blocklist(entry, 34);
    CHECK(grubslot_active(gpt, entry, &active) == -1, "nor the partition's entry sector");
    set_blocklist(entry, 1040);
    CHECK(grubslot_active(gpt, entry, &active) == -1, "nor the last sector of its tail A");
    CHECK(grubslot_other(gpt, mbr->tail_b) == 0, "and the gap's tail has no other in it");

    stamp_core(entry, 1008);
    set_blocklist(entry, 35);
    CHECK(grubslot_active(gpt, entry, &active) == -1,
          "an image longer than the partition's tail leaves no inactive tail");
    set_blocklist(entry, mbr->tail_a);
    CHECK(grubslot_active(mbr, entry, &active) == 0, "while the same count fits the gap's");
}

/**
 * Check `grubslot_patch` on a candidate: it writes the two fields GRUB reads,
 * the blocklist's first sector and its count, and nothing else; it holds an
 * image to each window's tail, where a gap's tail of 1023 sectors takes a
 * whole image of 1024 and not 1025, and a partition's tail of 1006 takes
 * 1007 and not 1008; it refuses an image of one sector or none; and it
 * refuses a target that is not a tail's first sector, as the gap's tail is
 * not in the partition.
 */
static void patches(const struct grubslot_window *mbr, const struct grubslot_window *gpt)
{
    make_core(image, 309);
    image[0] = 0xa5;
    image[GRUBSLOT_BLOCKLIST_LBA_OFF - 1] = 0xa5;
    CHECK(grubslot_patch(mbr, image, 309, mbr->tail_b, err, sizeof(err)) == 0,
          "a 309-sector image patches for tail B");
    CHECK(get_blocklist(image) == mbr->tail_b, "the blocklist names tail B");
    CHECK(image[GRUBSLOT_BLOCKLIST_LEN_OFF] == (unsigned char)(308 & 0xff) &&
              image[GRUBSLOT_BLOCKLIST_LEN_OFF + 1] == (unsigned char)((308 >> 8) & 0xff),
          "and its count is the sectors after the entry sector");
    CHECK(image[0] == 0xa5 && image[GRUBSLOT_BLOCKLIST_LBA_OFF - 1] == 0xa5,
          "the bytes before the blocklist are untouched");
    CHECK(image[GRUBSLOT_BLOCKLIST_SEG_OFF] == (unsigned char)(GRUBSLOT_BLOCKLIST_SEG & 0xff) &&
              image[GRUBSLOT_BLOCKLIST_SEG_OFF + 1] ==
                  (unsigned char)((GRUBSLOT_BLOCKLIST_SEG >> 8) & 0xff),
          "and the segment after it, which GRUB's boot code reads, is left alone");

    make_core(image, GRUBSLOT_MAX_IMAGE_SECTORS);
    CHECK(grubslot_patch(mbr, image, GRUBSLOT_MAX_IMAGE_SECTORS, mbr->tail_a, err, sizeof(err)) ==
              0,
          "an image of exactly 1024 sectors fits a tail");
    CHECK(grubslot_patch(mbr, image, GRUBSLOT_MAX_IMAGE_SECTORS + 1, mbr->tail_a, err,
                         sizeof(err)) == -1,
          "one sector more does not");
    CHECK(strstr(err, "does not fit") != NULL, "and says so");
    CHECK(grubslot_patch(mbr, image, 1, mbr->tail_a, err, sizeof(err)) == -1,
          "an image of one sector has no tail and is refused");
    CHECK(grubslot_patch(mbr, image, 0, mbr->tail_a, err, sizeof(err)) == -1,
          "and neither has an empty one");
    make_core(image, 300);
    CHECK(grubslot_patch(mbr, image, 300, 3, err, sizeof(err)) == -1,
          "a target that is not a tail's first sector is refused");
    CHECK(strstr(err, "not a tail slot") != NULL, "and says so");

    make_core(image, 1007);
    CHECK(grubslot_patch(gpt, image, 1007, 1041, err, sizeof(err)) == 0 &&
              get_blocklist(image) == 1041,
          "an image of 1007 sectors fits the partition's tail");
    make_core(image, 1008);
    CHECK(grubslot_patch(gpt, image, 1008, 35, err, sizeof(err)) == -1, "one of 1008 does not");
    CHECK(strstr(err, "does not fit a tail slot") != NULL, "and says so by name");
    make_core(image, 330);
    CHECK(grubslot_patch(gpt, image, 330, mbr->tail_b, err, sizeof(err)) == -1,
          "the gap's tail is not a target in the partition");
    CHECK(strstr(err, "not a tail slot") != NULL, "and says so");
}

/**
 * Check installs over a model of the BIOS boot partition: the tail not in
 * use is written whole, the entry sector at LBA 34 names it, and no sector
 * outside LBA 34 through 2046 is read or written; the sectors before the
 * partition are the GPT's and LBA 2047 belongs to no tail. An install goes
 * from tail A to B and back. An image of 1008 sectors, which the gap would
 * take, is refused before any write, and a dropped write to the tail is
 * caught with the old image still selected.
 */
static void gpt_installs(const struct grubslot_window *gpt)
{
    uint32_t target;

    model_reset(gpt, 35);
    make_core(image, 330);
    CHECK(grubslot_install(gpt, NULL, model_io, image, 330, entry, &target, err, sizeof(err)) == 0,
          "a 330-sector image installs in the BIOS boot partition");
    CHECK(target == 1041, "into tail B, the one not in use");
    CHECK(get_blocklist(gap + 34 * 512) == 1041, "and the entry sector at LBA 34 names it");
    CHECK(memcmp(gap + 1041 * 512, image + 512, 329u * 512u) == 0,
          "the tail has every sector after the entry sector");
    CHECK(gap[(size_t)(1041 + 329) * 512] == 0 && gap[(size_t)2046 * 512 + 511] == 0,
          "the rest of the tail, to LBA 2046, is zeroed");
    CHECK(gap[35 * 512] == 0xa0, "the old tail is untouched");
    CHECK(strays == 0 && outside_untouched(34, 2047),
          "and nothing before LBA 34 or at LBA 2047 is read or written");

    model_reset(gpt, 1041);
    make_core(image, 330);
    CHECK(grubslot_install(gpt, NULL, model_io, image, 330, entry, &target, err, sizeof(err)) ==
                  0 &&
              target == 35 && get_blocklist(gap + 34 * 512) == 35,
          "and from tail B the install goes back to tail A");
    CHECK(gap[1041 * 512] == 0xa0 && strays == 0 && outside_untouched(34, 2047),
          "leaving tail B and everything outside the window as they were");

    model_reset(gpt, 35);
    make_core(image, 1008);
    CHECK(grubslot_install(gpt, NULL, model_io, image, 1008, entry, &target, err, sizeof(err)) ==
              -1,
          "an image of 1008 sectors is refused in the partition");
    CHECK(strstr(err, "does not fit a tail slot") != NULL, "by name");
    CHECK(writes == 0, "with nothing written");
    CHECK(get_blocklist(gap + 34 * 512) == 35, "and the entry sector as it was");

    model_reset(gpt, 35);
    make_core(image, 330);
    drops_from = 1041;
    CHECK(grubslot_install(gpt, NULL, model_io, image, 330, entry, &target, err, sizeof(err)) == -1,
          "a dropped write to the partition's tail is caught");
    CHECK(get_blocklist(gap + 34 * 512) == 35 && gap[35 * 512] == 0xa0,
          "and the old image stays selected");
}

/**
 * Run the rows of `shapes`, `selectors` and `patches`, then install over the
 * model disk. Returns the count of failed checks, at most 255.
 *
 * - An install over the gap puts the image into the tail not in use, zeroes
 *   the rest of that tail so nothing of a longer predecessor survives,
 *   leaves the tail in use and sector 0 alone, and leaves the entry sector
 *   naming the new tail; a second install goes back to tail A. Then the
 *   rows of `gpt_installs`.
 * - A disk that reports a write as done and drops it: the readback catches
 *   it, and the entry sector still names the old tail, with the old image
 *   under it, so the machine boots what it booted before.
 * - Refusals before any write, which leave the entry sector as it was: an
 *   image of 1025 sectors; a file that is not a core image; and a core image
 *   cut short. A machine that activates either of the last two cannot boot
 *   and cannot be repaired remotely, since GRUB is what loads recovery.
 * - `grubslot_prepare` rounds a partial last sector up and zeroes the bytes
 *   past the file's end. Nothing in a core image records its byte length,
 *   so an image of 157703 bytes, which declares 308 tail sectors, still
 *   rounds to 309 sectors and is taken with up to six bytes cut off; cut by
 *   seven it is 308 sectors and refused. It refuses an empty file, one byte
 *   past a whole image, and a length past the buffer, and takes a whole
 *   image and a length that fills the buffer. A length near the end of the
 *   32-bit range would round to zero sectors and pass every bound, so the
 *   byte count is judged before it is rounded.
 * - A running image whose count runs out of one tail into the other uses
 *   both, so neither is free; calling the far one inactive would overwrite
 *   the running boot loader, and the install is refused. A count of zero
 *   names no image.
 * - GRUB walks the blocklist records downward from 0x1f4 until a count of
 *   zero ends the list, so a second record names sectors this code never
 *   reads. A running image with one, and a candidate with one, which would
 *   send the next boot to an extent nothing here wrote, are refused, as is
 *   an entry sector naming neither tail.
 */
int main(void)
{
    struct grubslot_window mbr, gpt;
    uint32_t active, target, cut;

    shapes(&mbr, &gpt);
    selectors(&mbr, &gpt);
    patches(&mbr, &gpt);

    model_reset(&mbr, mbr.tail_a);
    make_core(image, 309);
    CHECK(grubslot_install(&mbr, NULL, model_io, image, 309, entry, &target, err, sizeof(err)) == 0,
          "a 309-sector image installs");
    CHECK(target == mbr.tail_b, "into the tail that was not in use");
    CHECK(get_blocklist(gap + 512) == mbr.tail_b, "and the entry sector names it");
    CHECK(memcmp(gap + (size_t)mbr.tail_b * 512, image + 512, 308u * 512u) == 0,
          "the tail has every sector after the entry sector");
    CHECK(gap[(size_t)(mbr.tail_b + 308u) * 512] == 0, "the rest of the slot is zeroed");
    CHECK(gap[(size_t)mbr.tail_a * 512] == 0xa0, "and the old tail is untouched");
    CHECK(strays == 0 && outside_untouched(1, 2048), "and sector 0 is neither read nor written");

    model_reset(&mbr, mbr.tail_b);
    make_core(image, 309);
    CHECK(grubslot_install(&mbr, NULL, model_io, image, 309, entry, &target, err, sizeof(err)) ==
                  0 &&
              target == mbr.tail_a,
          "and from tail B the install goes back to tail A");

    gpt_installs(&gpt);

    model_reset(&mbr, mbr.tail_a);
    make_core(image, 309);
    drops_from = (long)mbr.tail_b;
    CHECK(grubslot_install(&mbr, NULL, model_io, image, 309, entry, &target, err, sizeof(err)) ==
              -1,
          "a write reported as done and dropped is caught by the readback");
    CHECK(strstr(err, "did not read back what was written") != NULL, "and says so");
    CHECK(get_blocklist(gap + 512) == mbr.tail_a,
          "the entry sector still names the tail that was in use");
    CHECK(gap[(size_t)mbr.tail_a * 512] == 0xa0, "and the old image is still there");

    model_reset(&mbr, mbr.tail_a);
    CHECK(grubslot_install(&mbr, NULL, model_io, image, GRUBSLOT_MAX_IMAGE_SECTORS + 1u, entry,
                           &target, err, sizeof(err)) == -1,
          "an image of 1025 sectors is refused");
    CHECK(writes == 0, "with nothing written");
    CHECK(get_blocklist(gap + 512) == mbr.tail_a, "and the entry sector as it was");

    model_reset(&mbr, mbr.tail_a);
    memset(image, 0x5c, 309u * 512u);
    CHECK(grubslot_install(&mbr, NULL, model_io, image, 309, entry, &target, err, sizeof(err)) ==
              -1,
          "a file that is not a core image is refused");
    CHECK(strstr(err, "not a GRUB core image") != NULL, "and says so");
    CHECK(writes == 0, "with nothing written");

    model_reset(&mbr, mbr.tail_a);
    make_core(image, 309);
    CHECK(grubslot_install(&mbr, NULL, model_io, image, 208, entry, &target, err, sizeof(err)) ==
              -1,
          "a core image that arrived truncated is refused");
    CHECK(strstr(err, "not the length it declares") != NULL, "and says so");
    CHECK(writes == 0, "with nothing written");

    for (cut = 0; cut <= 7u; cut++) {
        uint32_t bytes = 157703u - cut;
        uint32_t rounded = 0;

        make_core(image, 309);
        CHECK(grubslot_prepare(image, (uint32_t)sizeof(image), bytes, &rounded, err, sizeof(err)) ==
                  0,
              "a partial last sector rounds up");
        CHECK(rounded == (cut < 7u ? 309u : 308u), "to the sector count the bytes reach");
        if (bytes % 512u != 0u) {
            CHECK(image[bytes] == 0 && image[rounded * 512u - 1u] == 0,
                  "and the bytes past the file's end are zeroed");
        }
        if (cut < 7u) {
            CHECK(grubslot_patch(&mbr, image, rounded, mbr.tail_a, err, sizeof(err)) == 0,
                  "a loss inside the last sector is not seen at all");
        } else {
            CHECK(grubslot_patch(&mbr, image, rounded, mbr.tail_a, err, sizeof(err)) == -1,
                  "and one whole sector short is refused");
            CHECK(strstr(err, "not the length it declares") != NULL, "and says so");
        }
    }

    {
        uint32_t rounded = 0;

        CHECK(grubslot_prepare(image, (uint32_t)sizeof(image), 0, &rounded, err, sizeof(err)) == -1,
              "an empty file is refused");
        CHECK(grubslot_prepare(image, (uint32_t)sizeof(image),
                               GRUBSLOT_MAX_IMAGE_SECTORS * 512u + 1u, &rounded, err,
                               sizeof(err)) == -1,
              "and one byte past a whole image is refused");
        CHECK(grubslot_prepare(image, (uint32_t)sizeof(image), GRUBSLOT_MAX_IMAGE_SECTORS * 512u,
                               &rounded, err, sizeof(err)) == 0 &&
                  rounded == GRUBSLOT_MAX_IMAGE_SECTORS,
              "while exactly a whole image is not");
        CHECK(grubslot_prepare(image, 4096u, 4097u, &rounded, err, sizeof(err)) == -1,
              "a length past the buffer is refused");
        CHECK(grubslot_prepare(image, 4096u, 4096u, &rounded, err, sizeof(err)) == 0 &&
                  rounded == 8u,
              "and one that exactly fills it is not");
        CHECK(grubslot_prepare(image, (uint32_t)sizeof(image), 0xffffffffu, &rounded, err,
                               sizeof(err)) == -1,
              "and a length that would wrap when rounded is refused");
    }

    model_reset(&mbr, mbr.tail_a);
    gap[512 + GRUBSLOT_BLOCKLIST_LEN_OFF] = (unsigned char)(1200u & 0xffu);
    gap[512 + GRUBSLOT_BLOCKLIST_LEN_OFF + 1] = (unsigned char)((1200u >> 8) & 0xffu);
    CHECK(grubslot_active(&mbr, gap + 512, &active) == -1,
          "a running image longer than a slot leaves no inactive tail");
    make_core(image, 309);
    CHECK(grubslot_install(&mbr, NULL, model_io, image, 309, entry, &target, err, sizeof(err)) ==
              -1,
          "and the install is refused");
    CHECK(writes == 0, "with nothing written");

    model_reset(&mbr, mbr.tail_a);
    gap[512 + GRUBSLOT_BLOCKLIST_LEN_OFF] = 0;
    gap[512 + GRUBSLOT_BLOCKLIST_LEN_OFF + 1] = 0;
    CHECK(grubslot_active(&mbr, gap + 512, &active) == -1, "a count of zero names no image");

    model_reset(&mbr, mbr.tail_a);
    gap[512 + GRUBSLOT_BLOCKLIST_NEXT_LEN_OFF] = 8;
    CHECK(grubslot_active(&mbr, gap + 512, &active) == -1, "a second blocklist record is refused");
    make_core(image, 309);
    CHECK(grubslot_install(&mbr, NULL, model_io, image, 309, entry, &target, err, sizeof(err)) ==
              -1,
          "and the install with it");
    CHECK(writes == 0, "with nothing written");

    model_reset(&mbr, mbr.tail_a);
    make_core(image, 309);
    image[GRUBSLOT_BLOCKLIST_NEXT_LEN_OFF] = 4;
    CHECK(grubslot_install(&mbr, NULL, model_io, image, 309, entry, &target, err, sizeof(err)) ==
              -1,
          "a candidate read through more than one extent is refused");
    CHECK(strstr(err, "more than one extent") != NULL, "and says so");
    CHECK(writes == 0, "with nothing written");

    model_reset(&mbr, mbr.tail_a);
    set_blocklist(gap + 512, 900);
    CHECK(grubslot_install(&mbr, NULL, model_io, image, 309, entry, &target, err, sizeof(err)) ==
              -1,
          "an entry sector naming neither tail is refused");
    CHECK(writes == 0, "with nothing written");

    if (fails == 0) {
        printf("grubslot-hosttest: result: pass: the gap's and the BIOS boot partition's tails\n");
    }
    return fails > 255 ? 255 : fails;
}
