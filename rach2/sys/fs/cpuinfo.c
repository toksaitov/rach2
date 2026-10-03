/*
 * cpuinfo.c: the text of /proc/cpuinfo, as cpuinfo.h describes it.
 */

#include "cpuinfo.h"

#include "../arch/x86/kcpu.h"
#include "../lib/kstring.h"

/* Linux's names of the feature bits (arch/x86/include/asm/cpufeatures.h),
 * one table for each register, in bit order; a bit Linux does not show is
 * NULL. The extended leaf's EDX repeats most of leaf 1's, which Linux shows
 * once. */
static const char *const leaf1_edx[32] = {
    "fpu",  "vme",  "de",   "pse", "tsc",  "msr", "pae",   "mce", "cx8",     "apic", 0,
    "sep",  "mtrr", "pge",  "mca", "cmov", "pat", "pse36", "pn",  "clflush", 0,      "dts",
    "acpi", "mmx",  "fxsr", "sse", "sse2", "ss",  "ht",    "tm",  "ia64",    "pbe"};

static const char *const ext1_edx[32] = {
    0,         0, 0, 0,          0,         0,        0, 0,    0,          0,      0,
    "syscall", 0, 0, 0,          0,         0,        0, 0,    "mp",       "nx",   0,
    "mmxext",  0, 0, "fxsr_opt", "pdpe1gb", "rdtscp", 0, "lm", "3dnowext", "3dnow"};

static const char *const leaf1_ecx[32] = {
    "pni",    "pclmulqdq", "dtes64", "monitor", "ds_cpl",
    "vmx",    "smx",       "est",    "tm2",     "ssse3",
    "cid",    "sdbg",      "fma",    "cx16",    "xtpr",
    "pdcm",   0,           "pcid",   "dca",     "sse4_1",
    "sse4_2", "x2apic",    "movbe",  "popcnt",  "tsc_deadline_timer",
    "aes",    "xsave",     0,        "avx",     "f16c",
    "rdrand", "hypervisor"};

static const char *const ext1_ecx[32] = {"lahf_lm",
                                         "cmp_legacy",
                                         "svm",
                                         "extapic",
                                         "cr8_legacy",
                                         "abm",
                                         "sse4a",
                                         "misalignsse",
                                         "3dnowprefetch",
                                         "osvw",
                                         "ibs",
                                         "xop",
                                         "skinit",
                                         "wdt",
                                         0,
                                         "lwp",
                                         "fma4",
                                         "tce",
                                         0,
                                         "nodeid_msr",
                                         0,
                                         "tbm",
                                         "topoext",
                                         "perfctr_core",
                                         "perfctr_nb",
                                         0,
                                         "bpext",
                                         "ptsc",
                                         "perfctr_llc",
                                         "mwaitx",
                                         0,
                                         0};

static const char *const leaf7_ebx[32] = {"fsgsbase", "tsc_adjust",
                                          "sgx",      "bmi1",
                                          "hle",      "avx2",
                                          0,          "smep",
                                          "bmi2",     "erms",
                                          "invpcid",  "rtm",
                                          "cqm",      0,
                                          "mpx",      "rdt_a",
                                          "avx512f",  "avx512dq",
                                          "rdseed",   "adx",
                                          "smap",     "avx512ifma",
                                          0,          "clflushopt",
                                          "clwb",     "intel_pt",
                                          "avx512pf", "avx512er",
                                          "avx512cd", "sha_ni",
                                          "avx512bw", "avx512vl"};

static const char *const leaf7_ecx[32] = {0,
                                          "avx512vbmi",
                                          "umip",
                                          "pku",
                                          "ospke",
                                          "waitpkg",
                                          "avx512_vbmi2",
                                          0,
                                          "gfni",
                                          "vaes",
                                          "vpclmulqdq",
                                          "avx512_vnni",
                                          "avx512_bitalg",
                                          "tme",
                                          "avx512_vpopcntdq",
                                          0,
                                          "la57",
                                          0,
                                          0,
                                          0,
                                          0,
                                          0,
                                          "rdpid",
                                          0,
                                          "bus_lock_detect",
                                          "cldemote",
                                          0,
                                          "movdiri",
                                          "movdir64b",
                                          "enqcmd",
                                          "sgx_lc",
                                          0};

struct text {
    char *out;
    uint32_t cap;
    uint32_t len;
};

static void put(struct text *t, const char *s)
{
    while (*s && t->len + 1 < t->cap) {
        t->out[t->len++] = *s++;
    }
    t->out[t->len] = '\0';
}

static void put_u32(struct text *t, uint32_t v)
{
    char digits[11];
    int n = 0;

    do {
        digits[n++] = (char)('0' + v % 10u);
        v /= 10u;
    } while (v);
    while (n > 0) {
        char c[2];

        c[0] = digits[--n];
        c[1] = '\0';
        put(t, c);
    }
}

static void put_field(struct text *t, const char *name, uint32_t v)
{
    put(t, name);
    put_u32(t, v);
    put(t, "\n");
}

static int same(const char *a, const char *b)
{
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

static void put_flags(struct text *t, const char *const names[32], uint32_t bits)
{
    int i;

    for (i = 0; i < 32; i++) {
        if ((bits & (1u << i)) && names[i]) {
            put(t, " ");
            put(t, names[i]);
        }
    }
}

/** The brand string of leaves 0x80000002 to 0x80000004, the spaces that pad
 * it at either end dropped, into `brand` of 49 bytes; empty when the
 * processor has none. */
static void brand_string(uint32_t ext_max, char *brand)
{
    uint32_t r[12];
    char *p = brand;
    uint32_t i;

    brand[0] = '\0';
    if (ext_max < 0x80000004u) {
        return;
    }
    for (i = 0; i < 3; i++) {
        kcpu_id(0x80000002u + i, &r[i * 4], &r[i * 4 + 1], &r[i * 4 + 2], &r[i * 4 + 3]);
    }
    k_memcpy(brand, r, 48);
    brand[48] = '\0';
    while (*p == ' ') {
        p++;
    }
    k_memmove(brand, p, k_strlen(p) + 1);
    for (p = brand + k_strlen(brand); p > brand && p[-1] == ' '; p--) {
        p[-1] = '\0';
    }
}

/**
 * The size in KB of the last-level cache: from leaf 4's descriptions, the
 * largest, on a processor that has the leaf, and otherwise from the extended
 * leaf 0x80000006, its L3 and then its L2; 0 when neither says.
 */
static uint32_t cache_kb(uint32_t max, uint32_t ext_max, int intel)
{
    uint32_t a, b, c, d, sub, best = 0;

    if (intel && max >= 4) {
        for (sub = 0; sub < 16; sub++) {
            uint32_t bytes;

            kcpu_id_sub(4, sub, &a, &b, &c, &d);
            if ((a & 0x1Fu) == 0) {
                break;
            }
            bytes = (((b >> 22) & 0x3FFu) + 1) * (((b >> 12) & 0x3FFu) + 1) * ((b & 0xFFFu) + 1) *
                    (c + 1);
            if (bytes / 1024u > best) {
                best = bytes / 1024u;
            }
        }
        if (best) {
            return best;
        }
    }
    if (ext_max >= 0x80000006u) {
        kcpu_id(0x80000006u, 0, 0, &c, &d);
        if ((d >> 18) != 0) {
            return (d >> 18) * 512u;
        }
        return c >> 16;
    }
    return 0;
}

uint32_t cpuinfo_text(char *out, uint32_t cap)
{
    struct text t;
    uint32_t max, ext_max, a, b, c, d, family, model;
    uint32_t e1c = 0, e1d = 0, l7b = 0, l7c = 0, cache;
    char vendor[13];
    char brand[49];

    if (cap == 0) {
        return 0;
    }
    t.out = out;
    t.cap = cap;
    t.len = 0;
    out[0] = '\0';
    kcpu_id(0, &max, &b, &c, &d);
    k_memcpy(vendor, &b, 4);
    k_memcpy(vendor + 4, &d, 4);
    k_memcpy(vendor + 8, &c, 4);
    vendor[12] = '\0';
    kcpu_id(0x80000000u, &ext_max, 0, 0, 0);
    if (ext_max < 0x80000000u || ext_max > 0x8000FFFFu) {
        ext_max = 0;
    }
    kcpu_id(1, &a, &b, &c, &d);
    family = (a >> 8) & 0xFu;
    model = (a >> 4) & 0xFu;
    if (family == 0xFu) {
        family += (a >> 20) & 0xFFu;
    }
    if (family == 6u || family >= 0xFu) {
        model += ((a >> 16) & 0xFu) << 4;
    }
    brand_string(ext_max, brand);

    put_field(&t, "processor\t: ", 0);
    put(&t, "vendor_id\t: ");
    put(&t, vendor);
    put(&t, "\n");
    put_field(&t, "cpu family\t: ", family);
    put_field(&t, "model\t\t: ", model);
    put(&t, "model name\t: ");
    put(&t, brand[0] ? brand : "unknown");
    put(&t, "\n");
    put_field(&t, "stepping\t: ", a & 0xFu);
    cache = cache_kb(max, ext_max, same(vendor, "GenuineIntel"));
    if (cache) {
        put(&t, "cache size\t: ");
        put_u32(&t, cache);
        put(&t, " KB\n");
    }
    put_field(&t, "physical id\t: ", 0);
    put_field(&t, "siblings\t: ", 1);
    put_field(&t, "core id\t\t: ", 0);
    put_field(&t, "cpu cores\t: ", 1);
    put_field(&t, "apicid\t\t: ", b >> 24);
    put_field(&t, "initial apicid\t: ", b >> 24);
    put(&t,
        (d & 1u) ? "fpu\t\t: yes\nfpu_exception\t: yes\n" : "fpu\t\t: no\nfpu_exception\t: no\n");
    put_field(&t, "cpuid level\t: ", max);
    put(&t, "wp\t\t: yes\nflags\t\t:");
    if (ext_max >= 0x80000001u) {
        kcpu_id(0x80000001u, 0, 0, &e1c, &e1d);
    }
    if (max >= 7) {
        kcpu_id_sub(7, 0, 0, &l7b, &l7c, 0);
    }
    put_flags(&t, leaf1_edx, d);
    put_flags(&t, ext1_edx, e1d);
    put_flags(&t, leaf1_ecx, c);
    put_flags(&t, ext1_ecx, e1c);
    put_flags(&t, leaf7_ebx, l7b);
    put_flags(&t, leaf7_ecx, l7c);
    put(&t, "\n");
    put_field(&t, "clflush size\t: ", ((b >> 8) & 0xFFu) * 8u);
    put_field(&t, "cache_alignment\t: ", ((b >> 8) & 0xFFu) * 8u);
    if (ext_max >= 0x80000008u) {
        kcpu_id(0x80000008u, &a, 0, 0, 0);
        put(&t, "address sizes\t: ");
        put_u32(&t, a & 0xFFu);
        put(&t, " bits physical, ");
        put_u32(&t, (a >> 8) & 0xFFu);
        put(&t, " bits virtual\n");
    }
    put(&t, "power management:\n\n");
    return t.len;
}
