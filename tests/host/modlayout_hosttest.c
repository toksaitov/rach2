/**
 * Seam test for the boot-module placement check (sys/mm/modlayout.c).
 *
 * host-deps:
 *
 * The check decides whether the kernel may reserve memory over what the
 * bootloader placed, and three of its four refusals name placements no
 * bootloader will produce on request: a module overlapping the kernel image,
 * two modules overlapping each other, and a range that wraps the address
 * space. They are crafted here instead. The accepted cases matter as much:
 * modules placed back to back, out of order, and exactly at the ceiling must
 * all pass, or a legal boot would panic. Returns the failed-check count
 * (0 == pass).
 */

#include "mm/modlayout.c"

static int fails;

/** Equal NUL-terminated strings? (no host string.h here; see the note below) */
static int streq(const char *a, const char *b)
{
    while (*a != '\0' && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

/* No stdio here: the kernel's kint.h and the host's headers disagree on
   uintptr_t, so a kernel-header test reports by exit code alone, as
   mbr_hosttest.c does. `what` documents the case at its call site. */
#define CHECK(cond, what) \
    do {                  \
        (void)(what);     \
        if (!(cond)) {    \
            fails++;      \
        }                 \
    } while (0)

#define KEND  0x00200000u
#define LIMIT 0x02000000u

int main(void)
{
    struct modlayout_range m[3];
    uint32_t bad = 99;

    /* One ordinary module above the kernel. */
    m[0].start = 0x00300000u;
    m[0].end = 0x00400000u;
    CHECK(modlayout_check(m, 1, KEND, LIMIT, &bad) == MODLAYOUT_OK, "one module");
    CHECK(bad == 0, "no index reported on success");

    /* Back-to-back modules share an edge and do not overlap: the ranges are
       half-open, so [a,b) beside [b,c) is legal and must pass. */
    m[0].start = 0x00300000u;
    m[0].end = 0x00400000u;
    m[1].start = 0x00400000u;
    m[1].end = 0x00500000u;
    CHECK(modlayout_check(m, 2, KEND, LIMIT, &bad) == MODLAYOUT_OK, "adjacent modules");

    /* Descending order is a placement, not an error. */
    m[0].start = 0x00500000u;
    m[0].end = 0x00600000u;
    m[1].start = 0x00300000u;
    m[1].end = 0x00400000u;
    CHECK(modlayout_check(m, 2, KEND, LIMIT, &bad) == MODLAYOUT_OK, "out-of-order modules");

    /* Ending exactly at the ceiling is inside it. */
    m[0].start = LIMIT - 0x1000u;
    m[0].end = LIMIT;
    CHECK(modlayout_check(m, 1, KEND, LIMIT, &bad) == MODLAYOUT_OK, "module ends at the limit");

    /* One byte past the ceiling is not. */
    m[0].end = LIMIT + 1u;
    CHECK(modlayout_check(m, 1, KEND, LIMIT, &bad) == MODLAYOUT_PAST_LIMIT,
          "module one byte past the limit");
    CHECK(bad == 0, "the past-limit index");

    /* A module the size of the oversized-initrd case. */
    m[0].start = 0x00300000u;
    m[0].end = 0x03000000u;
    CHECK(modlayout_check(m, 1, KEND, LIMIT, &bad) == MODLAYOUT_PAST_LIMIT, "oversized module");

    /* Empty and backwards ranges. */
    m[0].start = 0x00300000u;
    m[0].end = 0x00300000u;
    CHECK(modlayout_check(m, 1, KEND, LIMIT, &bad) == MODLAYOUT_EMPTY, "empty module");
    m[0].end = 0x00200000u;
    CHECK(modlayout_check(m, 1, KEND, LIMIT, &bad) == MODLAYOUT_EMPTY, "backwards module");

    /* Overlapping the kernel image, both wholly and by one byte. */
    m[0].start = 0x00100000u;
    m[0].end = 0x00180000u;
    CHECK(modlayout_check(m, 1, KEND, LIMIT, &bad) == MODLAYOUT_HITS_KERNEL,
          "module under the kernel end");
    m[0].start = KEND - 1u;
    m[0].end = KEND + 0x1000u;
    CHECK(modlayout_check(m, 1, KEND, LIMIT, &bad) == MODLAYOUT_HITS_KERNEL,
          "module starting one byte inside the kernel");
    m[0].start = KEND;
    m[0].end = KEND + 0x1000u;
    CHECK(modlayout_check(m, 1, KEND, LIMIT, &bad) == MODLAYOUT_OK,
          "module starting exactly at the kernel end");

    /* Two modules overlapping, in both orders and by one byte. */
    m[0].start = 0x00300000u;
    m[0].end = 0x00500000u;
    m[1].start = 0x00400000u;
    m[1].end = 0x00600000u;
    CHECK(modlayout_check(m, 2, KEND, LIMIT, &bad) == MODLAYOUT_OVERLAP, "modules overlap");
    CHECK(bad == 1, "the overlap names the second module");
    m[0].start = 0x00400000u;
    m[0].end = 0x00600000u;
    m[1].start = 0x00300000u;
    m[1].end = 0x00500000u;
    CHECK(modlayout_check(m, 2, KEND, LIMIT, &bad) == MODLAYOUT_OVERLAP,
          "modules overlap, reversed");
    m[0].start = 0x00300000u;
    m[0].end = 0x00400001u;
    m[1].start = 0x00400000u;
    m[1].end = 0x00500000u;
    CHECK(modlayout_check(m, 2, KEND, LIMIT, &bad) == MODLAYOUT_OVERLAP,
          "modules overlap by one byte");

    /* One module wholly inside another. */
    m[0].start = 0x00300000u;
    m[0].end = 0x00600000u;
    m[1].start = 0x00400000u;
    m[1].end = 0x00500000u;
    CHECK(modlayout_check(m, 2, KEND, LIMIT, &bad) == MODLAYOUT_OVERLAP, "module inside another");

    /* A third module overlapping only the first: the pairwise walk must reach
       the (0,2) pair, not stop after (0,1). */
    m[0].start = 0x00300000u;
    m[0].end = 0x00400000u;
    m[1].start = 0x00800000u;
    m[1].end = 0x00900000u;
    m[2].start = 0x00380000u;
    m[2].end = 0x00390000u;
    CHECK(modlayout_check(m, 3, KEND, LIMIT, &bad) == MODLAYOUT_OVERLAP,
          "non-adjacent pair overlaps");
    CHECK(bad == 2, "the non-adjacent overlap names the later module");

    /* A range that wraps the address space: end < start catches it before any
       arithmetic can wrap with it. */
    m[0].start = 0xFFFFF000u;
    m[0].end = 0x00001000u;
    CHECK(modlayout_check(m, 1, KEND, LIMIT, &bad) == MODLAYOUT_EMPTY, "wrapping range");

    /* No modules at all is a legal boot (the kernel-only case). */
    CHECK(modlayout_check(m, 0, KEND, LIMIT, &bad) == MODLAYOUT_OK, "no modules");

    /* The panic text carries the rule, the index and the range, because the
       target laptop has no serial port and k_panic clears the screen: what is
       not in this string is not seen on that machine. */
    {
        char msg[128];
        char tiny[8];

        modlayout_describe(MODLAYOUT_OVERLAP, 2, 0x00300000u, 0x00400000u, msg,
                           (uint32_t)sizeof(msg));
        CHECK(streq(msg, "two boot modules overlap: module 2 spans 0x00300000 - 0x00400000"),
              "the overlap description");

        modlayout_describe(MODLAYOUT_PAST_LIMIT, 0, 0x00197000u, 0x02997000u, msg,
                           (uint32_t)sizeof(msg));
        CHECK(streq(msg, "a boot module ends past the 32 MiB memory limit: module 0 "
                         "spans 0x00197000 - 0x02997000"),
              "the past-limit description");

        /* A buffer too small truncates and still terminates: a panic screen
           with a short message beats a kernel that ran off a buffer. */
        modlayout_describe(MODLAYOUT_OVERLAP, 1, 1u, 2u, tiny, (uint32_t)sizeof(tiny));
        CHECK(tiny[sizeof(tiny) - 1] == '\0', "the truncated description terminates");
        CHECK(streq(tiny, "two boo"), "the truncated description keeps its head");

        /* A zero-length buffer is not written at all. */
        tiny[0] = 'x';
        modlayout_describe(MODLAYOUT_OVERLAP, 1, 1u, 2u, tiny, 0);
        CHECK(tiny[0] == 'x', "a zero-length buffer is untouched");

        /* Decimal indices past one digit, and the zero case. */
        modlayout_describe(MODLAYOUT_EMPTY, 10, 0u, 0u, msg, (uint32_t)sizeof(msg));
        CHECK(streq(msg, "a boot module is empty or ends before it starts: module 10 "
                         "spans 0x00000000 - 0x00000000"),
              "a two-digit index and zero addresses");
    }

    /* Every code names itself, and an unknown code still returns a string. */
    CHECK(modlayout_reason(MODLAYOUT_OK) != 0, "OK has a reason");
    CHECK(modlayout_reason(MODLAYOUT_EMPTY) != 0, "EMPTY has a reason");
    CHECK(modlayout_reason(MODLAYOUT_PAST_LIMIT) != 0, "PAST_LIMIT has a reason");
    CHECK(modlayout_reason(MODLAYOUT_HITS_KERNEL) != 0, "HITS_KERNEL has a reason");
    CHECK(modlayout_reason(MODLAYOUT_OVERLAP) != 0, "OVERLAP has a reason");
    CHECK(modlayout_reason(99) != 0, "an unknown code has a reason");

    return fails;
}
