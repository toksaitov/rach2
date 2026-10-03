/*
 * grubslot_hosttest.c: the two core-image tails of a boot loader window, and
 * which one is in use.
 *
 * host-deps: utils/grubslot.c
 *
 * Sector 0 has the partition table, and a disk writes whole sectors, so the
 * boot code's pointer at byte 0x5c can never be rewritten without rewriting the
 * table beside it. The arithmetic here is what lets it stay untouched forever:
 * the window's first sector is a selector, the rest has two tails, and a
 * replacement goes into the one not in use. The window is the post-MBR gap of
 * an MBR disk, LBA 1 through 2047, or the BIOS boot partition of a GPT disk,
 * LBA 34 through 2047 on the disks this project builds. Getting the tail
 * boundaries or the blocklist offsets wrong produces a disk that does not boot
 * and cannot be repaired remotely, so every number below is checked, not
 * assumed.
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

/* The fields grub-mkimage writes into a core image's first sector: the segment
   the tail is read into, and the tail's length. */
static void stamp_core(unsigned char *p, unsigned long sectors)
{
    p[GRUBSLOT_BLOCKLIST_LEN_OFF] = (unsigned char)((sectors - 1u) & 0xffu);
    p[GRUBSLOT_BLOCKLIST_LEN_OFF + 1] = (unsigned char)(((sectors - 1u) >> 8) & 0xffu);
    p[GRUBSLOT_BLOCKLIST_SEG_OFF] = (unsigned char)(GRUBSLOT_BLOCKLIST_SEG & 0xffu);
    p[GRUBSLOT_BLOCKLIST_SEG_OFF + 1] = (unsigned char)((GRUBSLOT_BLOCKLIST_SEG >> 8) & 0xffu);
    p[GRUBSLOT_BLOCKLIST_NEXT_LEN_OFF] = 0;
    p[GRUBSLOT_BLOCKLIST_NEXT_LEN_OFF + 1] = 0;
}

/* A whole core image as grub-mkimage leaves one, `sectors` long. */
static void make_core(unsigned char *p, unsigned long sectors)
{
    memset(p, 0x5c, sectors * 512u);
    stamp_core(p, sectors);
}

static void set_blocklist(unsigned char *p, unsigned long long lba)
{
    int i;

    for (i = 0; i < 8; i++) {
        p[GRUBSLOT_BLOCKLIST_LBA_OFF + i] = (unsigned char)(lba & 0xffu);
        lba >>= 8;
    }
}

static unsigned long long get_blocklist(const unsigned char *p)
{
    unsigned long long v = 0;
    int i;

    for (i = 7; i >= 0; i--) {
        v = (v << 8) | (unsigned long long)p[GRUBSLOT_BLOCKLIST_LBA_OFF + i];
    }
    return v;
}

/* A model of the disk's first 2048 sectors, with an io that can be told to
   report a write as done without doing it. A disk that loses a write silently
   is the failure the readback exists for, and it cannot be staged on a real
   device. Every sector outside the window under test starts as 0xee, and an
   access outside the window is counted, so a stray read or write is caught
   whether or not it changes a byte. */
static unsigned char gap[2048 * 512];
static long drops_from = -1;
static int writes, strays;
static const struct grubslot_window *in_use;

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

/* One disk as it is before an install over the window `w`: an entry sector
   naming `active`, an old image under it whose first sector is recognisable,
   and 0x77 in the rest of both tails, so a fill that was not written shows. */
static void model_reset(const struct grubslot_window *w, unsigned long long active)
{
    size_t end = (size_t)w->tail_b + w->tail;

    /* A window laid out wrong may run past the model; the rows say so. */
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

/* Whether every sector of the model outside `[from, to)` still has 0xee. */
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
 * The windows' shapes: the MBR gap as it always was, the BIOS boot partition
 * the disk builder makes, a larger one capped at the longest tail, and the
 * windows no install could use.
 */
static void shapes(struct grubslot_window *mbr, struct grubslot_window *gpt)
{
    struct grubslot_window w;

    /* The gap's shape. A tail is 1023 sectors and the two are adjacent and
       disjoint, so between them they cover LBA 2 through 2047 and nothing
       else: LBA 0 is the MBR, LBA 1 is the selector. */
    CHECK(grubslot_window(1, 2047, mbr, err, sizeof(err)) == 0, "the post-MBR gap is a window");
    CHECK(mbr->entry == 1 && mbr->tail_a == 2, "tail A starts at LBA 2, just past the selector");
    CHECK(mbr->tail == GRUBSLOT_TAIL_SECTORS && mbr->tail_b == mbr->tail_a + mbr->tail,
          "tail B starts where tail A ends");
    CHECK(mbr->tail_b + mbr->tail == 2048, "and tail B ends where the first partition begins");
    CHECK(GRUBSLOT_MAX_IMAGE_SECTORS == GRUBSLOT_TAIL_SECTORS + 1,
          "a whole image is its entry sector plus a tail");

    /* The BIOS boot partition of the disk builder and the installer, LBA 34
       through 2047: 2014 sectors, so two tails of 1006 and one sector left
       over, which nothing writes. */
    CHECK(grubslot_window(34, 2014, gpt, err, sizeof(err)) == 0,
          "the BIOS boot partition is a window");
    CHECK(gpt->entry == 34 && gpt->tail_a == 35,
          "its entry sector is the partition's first, and tail A follows it");
    CHECK(gpt->tail == 1006 && gpt->tail_b == 1041, "two tails of 1006 sectors, B at LBA 1041");
    CHECK(gpt->tail_b + gpt->tail == 2047, "and B ends one sector before the partition does");

    /* A partition of a mebibyte has tails of 1023, and a larger one is capped
       there, since a tail is written whole on every install. */
    CHECK(grubslot_window(2048, 2048, &w, err, sizeof(err)) == 0 && w.tail == 1023 &&
              w.tail_b == 2048 + 1 + 1023,
          "a 1 MiB partition has tails of 1023");
    CHECK(grubslot_window(2048, 1u << 20, &w, err, sizeof(err)) == 0 &&
              w.tail == GRUBSLOT_TAIL_SECTORS,
          "and a larger one is capped at the longest tail");

    /* What the kernel would never publish, refused all the same. */
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
 * Which tail an entry sector names, in the window it is read in. The same LBA
 * is a tail in one window and nothing in the other.
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

    /* An entry sector whose extent does not begin at a tail boundary. This is a
       check on the layout and not on who built the disk: a conventional GRUB
       installation, whose one extent also begins at LBA 2, passes it. */
    set_blocklist(entry, 3);
    CHECK(grubslot_active(mbr, entry, &active) == -1, "a blocklist inside a tail names no slot");
    set_blocklist(entry, 0);
    CHECK(grubslot_active(mbr, entry, &active) == -1, "nor does one naming the MBR");
    set_blocklist(entry, 1);
    CHECK(grubslot_active(mbr, entry, &active) == -1, "nor one naming the selector itself");
    set_blocklist(entry, 2048);
    CHECK(grubslot_active(mbr, entry, &active) == -1, "nor one past the gap");
    CHECK(grubslot_other(mbr, 0) == 0, "and there is no other tail to a slot that is not one");

    /* The GPT window: the blocklist grubembed writes names LBA 35, one past
       the partition's first sector, and an install moves it to 1041. */
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

    /* A running image of 1007 tail sectors fits the gap's tail and runs out of
       the partition's tail A into its B. */
    stamp_core(entry, 1008);
    set_blocklist(entry, 35);
    CHECK(grubslot_active(gpt, entry, &active) == -1,
          "an image longer than the partition's tail leaves no inactive tail");
    set_blocklist(entry, mbr->tail_a);
    CHECK(grubslot_active(mbr, entry, &active) == 0, "while the same count fits the gap's");
}

/**
 * Patching a candidate: the two fields GRUB reads, the bounds of each window,
 * and the targets that are not tails.
 */
static void patches(const struct grubslot_window *mbr, const struct grubslot_window *gpt)
{
    /* The patch writes the two fields GRUB reads, and nothing else. */
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

    /* The bounds. A tail has 1023 sectors, so a whole image of 1024 fills one
       exactly and 1025 does not fit. */
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

    /* The partition's tail has 1006 sectors, so an image of 1007 fills one and
       1008, which the gap would take, does not fit. */
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
 * Installing over a model of the BIOS boot partition: the tail not in use is
 * written whole, the entry sector at LBA 34 is rewritten last, and no sector
 * outside LBA 34 through 2046 is read or written; the sectors before the
 * partition are the GPT's and LBA 2047 belongs to no tail.
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

    /* An image the gap would take and the partition cannot. */
    model_reset(gpt, 35);
    make_core(image, 1008);
    CHECK(grubslot_install(gpt, NULL, model_io, image, 1008, entry, &target, err, sizeof(err)) ==
              -1,
          "an image of 1008 sectors is refused in the partition");
    CHECK(strstr(err, "does not fit a tail slot") != NULL, "by name");
    CHECK(writes == 0, "with nothing written");
    CHECK(get_blocklist(gap + 34 * 512) == 35, "and the entry sector as it was");

    /* A dropped write is caught there as in the gap. */
    model_reset(gpt, 35);
    make_core(image, 330);
    drops_from = 1041;
    CHECK(grubslot_install(gpt, NULL, model_io, image, 330, entry, &target, err, sizeof(err)) == -1,
          "a dropped write to the partition's tail is caught");
    CHECK(get_blocklist(gap + 34 * 512) == 35 && gap[35 * 512] == 0xa0,
          "and the old image stays selected");
}

int main(void)
{
    struct grubslot_window mbr, gpt;
    uint32_t active, target, cut;

    shapes(&mbr, &gpt);
    selectors(&mbr, &gpt);
    patches(&mbr, &gpt);

    /* Installing over a model disk. The image goes into the tail that is not in
       use, the rest of that slot is zeroed so nothing of a longer predecessor
       survives, the tail that was in use is not touched, and the entry sector
       is left naming the new one. */
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

    /* A disk that reports a write as done and drops it. The readback must catch
       it, and what the machine boots must be what it booted before: the entry
       sector still naming the old tail, with the old image still under it. */
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

    /* A refusal costs nothing: the image is judged before any write. */
    model_reset(&mbr, mbr.tail_a);
    CHECK(grubslot_install(&mbr, NULL, model_io, image, GRUBSLOT_MAX_IMAGE_SECTORS + 1u, entry,
                           &target, err, sizeof(err)) == -1,
          "an image of 1025 sectors is refused");
    CHECK(writes == 0, "with nothing written");
    CHECK(get_blocklist(gap + 512) == mbr.tail_a, "and the entry sector as it was");

    /* A file that is not a core image, and one that is a core image cut short.
       Both are refused before any write, because a machine that activates
       either one cannot boot and cannot be repaired remotely: GRUB is what
       loads recovery. */
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

    /* The boundary of that check, over the rounding the tool really does. A
       core image is not a whole number of sectors: the one this project builds
       is 157703 bytes and declares 308 tail sectors. Cutting up to six bytes
       off it still rounds to 309 sectors and is accepted, because nothing in
       the image records its byte length; cutting seven crosses into 308 and is
       refused. Both halves are asserted so that neither is assumed. */
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

    /* And the sizes that have no image in them at all. */
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
        /* Rounding a length near the end of the range wraps to zero sectors,
           and a wrapped count passes every bound there is, so the byte count is
           judged before it is rounded. */
        CHECK(grubslot_prepare(image, (uint32_t)sizeof(image), 0xffffffffu, &rounded, err,
                               sizeof(err)) == -1,
              "and a length that would wrap when rounded is refused");
    }

    /* An entry sector whose count runs out of one tail and into the other. The
       image in use occupies both, so neither is free and there is nothing to
       install into; calling the far one inactive would overwrite the running
       boot loader. */
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

    /* A loader read through more than one extent. GRUB walks the records
       downward from 0x1f4 until a count of zero ends the list, so a second one
       names sectors this code never looks at, anywhere in the window. */
    model_reset(&mbr, mbr.tail_a);
    gap[512 + GRUBSLOT_BLOCKLIST_NEXT_LEN_OFF] = 8;
    CHECK(grubslot_active(&mbr, gap + 512, &active) == -1, "a second blocklist record is refused");
    make_core(image, 309);
    CHECK(grubslot_install(&mbr, NULL, model_io, image, 309, entry, &target, err, sizeof(err)) ==
              -1,
          "and the install with it");
    CHECK(writes == 0, "with nothing written");

    /* And the candidate is held to the same one-extent rule the running image
       is. A file with a second record would send the next boot to an extent
       nothing here wrote, and there would be no way back. */
    model_reset(&mbr, mbr.tail_a);
    make_core(image, 309);
    image[GRUBSLOT_BLOCKLIST_NEXT_LEN_OFF] = 4;
    CHECK(grubslot_install(&mbr, NULL, model_io, image, 309, entry, &target, err, sizeof(err)) ==
              -1,
          "a candidate read through more than one extent is refused");
    CHECK(strstr(err, "more than one extent") != NULL, "and says so");
    CHECK(writes == 0, "with nothing written");

    /* And an extent beginning nowhere near a tail is not written over
     * either. */
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
