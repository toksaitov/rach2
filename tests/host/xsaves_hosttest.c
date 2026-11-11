/**
 * xsaves_hosttest.c: whether XSAVES may be used (`kcpu_xsaves_usable`,
 * `arch/x86/xsaves.c`), over a modelled `cpuid` and microcode revision.
 *
 * Rows: a part without leaf 0xD, or whose subleaf 1 lacks XSAVES, may not;
 * an Intel part may, its microcode unread; of AMD's family 0x17, a Zen 2
 * part Linux 6.12's erratum_1386_microcode does not name may not at any
 * revision, the two parts it names may at their fixed revision and above
 * and not below it, and not at another stepping; a family 0x17 model that
 * is neither Zen 1 nor Zen 2, and a Zen 3 part, may. Prints each failure;
 * the exit status is the number of failed checks, at most 255.
 */

#include "../../rach2/sys/arch/x86/xsaves.c"

extern int printf(const char *fmt, ...);

static int fails;

/** Count and print a failure when `cond` is false; `what` says what held. */
#define CHECK(cond, what)                 \
    do {                                  \
        if (!(cond)) {                    \
            printf("  FAIL: %s\n", what); \
            fails++;                      \
        }                                 \
    } while (0)

/* The modelled processor: its vendor's three registers of leaf 0 in EBX,
 * EDX and ECX order, its highest leaf, leaf 1's signature, leaf 0xD
 * subleaf 1's EAX, and MSR_AMD64_PATCH_LEVEL; `msr_reads` counts the reads
 * of that register. */
static uint32_t vendor[3];
static uint32_t max_leaf;
static uint32_t signature;
static uint32_t d1_eax;
static uint32_t patch_level;
static int msr_reads;

/** Store `v` through `p` unless `p` is NULL. */
static void put(uint32_t *p, uint32_t v)
{
    if (p) {
        *p = v;
    }
}

/** The model's leaves 0, 1 and 0xD; any other reads as zero. */
void kcpu_id_sub(uint32_t leaf, uint32_t sub, uint32_t *a, uint32_t *b, uint32_t *c, uint32_t *d)
{
    uint32_t ra = 0, rb = 0, rc = 0, rd = 0;

    if (leaf == 0) {
        ra = max_leaf;
        rb = vendor[0];
        rd = vendor[1];
        rc = vendor[2];
    } else if (leaf == 1 && max_leaf >= 1) {
        ra = signature;
    } else if (leaf == 0xD && sub == 1 && max_leaf >= 0xD) {
        ra = d1_eax;
    }
    put(a, ra);
    put(b, rb);
    put(c, rc);
    put(d, rd);
}

/** Subleaf 0 of `leaf`. */
void kcpu_id(uint32_t leaf, uint32_t *a, uint32_t *b, uint32_t *c, uint32_t *d)
{
    kcpu_id_sub(leaf, 0, a, b, c, d);
}

/** MSR_AMD64_PATCH_LEVEL alone, the revision in its low half. */
void kcpu_rdmsr(uint32_t msr, uint32_t *lo, uint32_t *hi)
{
    CHECK(msr == 0x8Bu, "only MSR_AMD64_PATCH_LEVEL is read");
    msr_reads++;
    put(lo, patch_level);
    put(hi, 0);
}

/** An AMD part of `family`, `model` and `stepping`, with leaf 0xD and
 * XSAVES, at microcode revision `revision`. */
static void amd(uint32_t family, uint32_t model, uint32_t stepping, uint32_t revision)
{
    vendor[0] = 0x68747541u; /* "Auth" */
    vendor[1] = 0x69746E65u; /* "enti" */
    vendor[2] = 0x444D4163u; /* "cAMD" */
    max_leaf = 0x10;
    signature = ((model >> 4) << 16) | ((model & 0xFu) << 4) | stepping;
    if (family >= 0xFu) {
        signature |= (0xFu << 8) | ((family - 0xFu) << 20);
    } else {
        signature |= family << 8;
    }
    d1_eax = 0xF; /* XSAVEOPT, XSAVEC, XGETBV1 and XSAVES */
    patch_level = revision;
    msr_reads = 0;
}

/** Run the rows; the count of failures, at most 255. */
int main(void)
{
    amd(0x17, 0x31, 0x0, 0x08301052u);
    max_leaf = 0xC;
    CHECK(!kcpu_xsaves_usable(), "a part without leaf 0xD may not use XSAVES");
    amd(0x17, 0x31, 0x0, 0x08301052u);
    d1_eax = 0x7;
    CHECK(!kcpu_xsaves_usable(), "nor one whose subleaf 1 lacks it");

    amd(0x6, 0x3C, 0x3, 0);
    vendor[0] = 0x756E6547u; /* "Genu" */
    vendor[1] = 0x49656E69u; /* "ineI" */
    vendor[2] = 0x6C65746Eu; /* "ntel" */
    CHECK(kcpu_xsaves_usable() && msr_reads == 0,
          "an Intel part with it may, and its microcode is not read");

    amd(0x17, 0x71, 0x0, 0xFFFFFFFFu);
    CHECK(!kcpu_xsaves_usable() && msr_reads == 1,
          "a Zen 2 part the erratum's table does not name may not, at any revision");
    amd(0x17, 0xA0, 0x0, 0xFFFFFFFFu);
    CHECK(!kcpu_xsaves_usable(), "nor may a Zen 2 part of model 0xA0");

    amd(0x17, 0x31, 0x0, 0x08301052u);
    CHECK(kcpu_xsaves_usable(), "Zen 2 model 0x31 stepping 0 may at revision 0x08301052");
    amd(0x17, 0x31, 0x0, 0x08301053u);
    CHECK(kcpu_xsaves_usable(), "and above it");
    amd(0x17, 0x31, 0x0, 0x08301051u);
    CHECK(!kcpu_xsaves_usable(), "but not below it");
    amd(0x17, 0x31, 0x1, 0xFFFFFFFFu);
    CHECK(!kcpu_xsaves_usable(), "nor at another stepping");

    amd(0x17, 0x01, 0x2, 0x0800126Eu);
    CHECK(kcpu_xsaves_usable(), "Zen 1 model 0x01 stepping 2 may at revision 0x0800126E");
    amd(0x17, 0x01, 0x2, 0x0800126Du);
    CHECK(!kcpu_xsaves_usable(), "but not below it");

    amd(0x17, 0x80, 0x0, 0);
    CHECK(kcpu_xsaves_usable() && msr_reads == 0,
          "a family 0x17 model neither Zen 1 nor Zen 2 may, its microcode unread");
    amd(0x19, 0x21, 0x0, 0);
    CHECK(kcpu_xsaves_usable() && msr_reads == 0, "and so may a Zen 3 part");
    return fails > 255 ? 255 : fails;
}
