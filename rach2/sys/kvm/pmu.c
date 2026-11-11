/* Guest performance counters, their event filters and hardware switching. */

#include "kvm.h"

#include "../arch/arch.h"
#include "../arch/x86/kcpu.h"
#include "../lib/kerrno.h"
#include "../lib/kirq.h"
#include "../lib/kstring.h"
#include "../mm/kheap.h"
#include "../mm/usercopy.h"
#include "../watchdog.h"

#define PMC0          0x0C1u
#define EVENTSEL0     0x186u
#define PERF_CAP      0x345u
#define FIXED0        0x309u
#define FIXED_CTRL    0x38Du
#define GLOBAL_STATUS 0x38Eu
#define GLOBAL_CTRL   0x38Fu
#define GLOBAL_OVF    0x390u
#define FULL_PMC0     0x4C1u
#define LBR_SELECT    0x1C8u
#define LBR_TOS       0x1C9u
#define DEBUGCTL      0x1D9u
#define LBR_FROM      0x680u
#define LBR_TO        0x6C0u
#define LBR_INFO      0xDC0u
#define AMD_CTL0      0xC0010000u
#define AMD_CTR0      0xC0010004u
#define AMD_CORE_CTL0 0xC0010200u
#define AMD_CORE_CTR0 0xC0010201u
#define AMD_STATUS    0xC0000300u
#define AMD_GLOBAL    0xC0000301u
#define AMD_CLEAR     0xC0000302u

#define EV_USR         (1ull << 16)
#define EV_OS          (1ull << 17)
#define EV_EDGE        (1ull << 18)
#define EV_PIN         (1ull << 19)
#define EV_INT         (1ull << 20)
#define EV_ENABLE      (1ull << 22)
#define EV_INVERT      (1ull << 23)
#define EV_CMASK       (0xFFull << 24)
#define EV_GUEST       (1ull << 40)
#define EV_TX          (1ull << 32)
#define EV_TXCP        (1ull << 33)
#define SMM_FREEZE     (1ull << 12)
#define FW_WRITES      (1ull << 13)
#define LBR_FORMAT     0x3Full
#define DEBUGCTL_LBR   (1ull << 0)
#define DEBUGCTL_BTF   (1ull << 1)
#define DEBUGCTL_BLD   (1ull << 2)
#define DEBUGCTL_FRZ   (1ull << 11)
#define DEBUGCTL_RTM   (1ull << 15)
#define L7_RTM         (1u << 11)
#define L7_BLD         (1u << 24)
#define PDCM           (1u << 15)
#define CORE_EXT       (1u << 23)
#define L7_HYBRID      (1u << 15)
#define EV_RAW         (0xFFFFull | EV_EDGE | EV_INVERT | EV_CMASK)
#define AMD_MERGE      (0xF000000FFull | EV_ENABLE)
#define FIXED_BIT(j)   (1ull << (32u + (j)))
#define FIXED_PMI      8ull
#define AMD_EVENT      0xF000000FFull
#define FILTER_EXCLUDE (1ull << 55)
#define FILTER_MASK    (0xFFull << 56)
#define FILTER_MAX     300u

/*
 * A row of Linux's event constraints (arch/x86/events/perf_event.h:359-460):
 * a selector whose bits under `match` lie in `code` to `code + size` may
 * run on the counters of `mask`, general ones in its low 32 bits and fixed
 * counter j at bit 32 + j. The first row that matches is the event's; an
 * event no row matches may run on any general counter.
 */
struct constraint {
    uint64_t match;
    uint16_t code, size;
    uint64_t mask;
};

/* The rows' match masks: the event code, the code and unit mask, the code
 * and the unit-mask bits of `code`, and the whole of a fixed event, which
 * edge, invert, a counter mask or a TSX flag keeps off its fixed counter
 * (perf_event.h:413-460, FIXED_EVENT_FLAGS). */
#define M_EVENT 0xFFull
#define M_UMASK 0xFFFFull
#define M_FIXED (EV_RAW | EV_TX | EV_TXCP)

#define C_EVENT(c, n)    {M_EVENT, (c), 0, (n)}
#define C_RANGE(c, e, n) {M_EVENT, (c), (e) - (c), (n)}
#define C_UEVENT(c, n)   {M_UMASK, (c), 0, (n)}
#define C_UBIT(c, n)     {M_EVENT | (c), (c), 0, (n)}
#define C_FIXED(c, j)    {M_FIXED, (c), 0, 1ull << (32 + (j))}
#define C_METRIC(c)      {M_UMASK, (c), 0, 0}
#define C_END            {0, 0, 0, 0}

/* Linux 6.12.115's tables of arch/x86/events/intel/core.c, by its lines:
 * Yonah's (44), Core 2's (55), Nehalem's (73), Westmere's (97), Sandy
 * Bridge's (109), Ivy Bridge's (136), the generic version 2's (178),
 * Silvermont's and its successors' (207), Gracemont's (215), Skylake's
 * (234), Ice Lake's (286), Golden Cove's (332), Haswell's (504) and
 * Broadwell's (530). A metric event runs on no counter KVM has. */
static const struct constraint c_core[] = {C_EVENT(0x11, 0x2),
                                           C_EVENT(0x12, 0x2),
                                           C_EVENT(0x13, 0x2),
                                           C_EVENT(0x14, 0x1),
                                           C_EVENT(0x19, 0x2),
                                           C_EVENT(0xc1, 0x1),
                                           C_END};
static const struct constraint c_core2[] = {C_FIXED(0x00c0, 0), C_FIXED(0x003c, 1),
                                            C_FIXED(0x0300, 2), C_EVENT(0x10, 0x1),
                                            C_EVENT(0x11, 0x2), C_EVENT(0x12, 0x2),
                                            C_EVENT(0x13, 0x2), C_EVENT(0x14, 0x1),
                                            C_EVENT(0x18, 0x1), C_EVENT(0x19, 0x2),
                                            C_EVENT(0xa1, 0x1), C_EVENT(0xc9, 0x1),
                                            C_EVENT(0xcb, 0x1), C_END};
static const struct constraint c_nehalem[] = {
    C_FIXED(0x00c0, 0), C_FIXED(0x003c, 1), C_FIXED(0x0300, 2), C_EVENT(0x40, 0x3),
    C_EVENT(0x41, 0x3), C_EVENT(0x42, 0x3), C_EVENT(0x43, 0x3), C_EVENT(0x48, 0x3),
    C_EVENT(0x4e, 0x3), C_EVENT(0x51, 0x3), C_EVENT(0x63, 0x3), C_END};
static const struct constraint c_westmere[] = {
    C_FIXED(0x00c0, 0), C_FIXED(0x003c, 1), C_FIXED(0x0300, 2), C_EVENT(0x51, 0x3),
    C_EVENT(0x60, 0x1), C_EVENT(0x63, 0x3), C_EVENT(0xb3, 0x1), C_END};
static const struct constraint c_snb[] = {C_FIXED(0x00c0, 0),
                                          C_FIXED(0x003c, 1),
                                          C_FIXED(0x0300, 2),
                                          C_UEVENT(0x04a3, 0xf),
                                          C_UEVENT(0x05a3, 0xf),
                                          C_UEVENT(0x02a3, 0x4),
                                          C_UEVENT(0x06a3, 0x4),
                                          C_EVENT(0x48, 0x4),
                                          C_UEVENT(0x01c0, 0x2),
                                          C_EVENT(0xcd, 0x8),
                                          C_UEVENT(0x04a3, 0xf),
                                          C_UEVENT(0x02a3, 0x4),
                                          C_EVENT(0xd0, 0xf),
                                          C_EVENT(0xd1, 0xf),
                                          C_EVENT(0xd2, 0xf),
                                          C_EVENT(0xd3, 0xf),
                                          C_END};
static const struct constraint c_ivb[] = {
    C_FIXED(0x00c0, 0),    C_FIXED(0x003c, 1),    C_FIXED(0x0300, 2),
    C_UEVENT(0x0148, 0x4), C_UEVENT(0x0279, 0xf), C_UEVENT(0x019c, 0xf),
    C_UEVENT(0x02a3, 0xf), C_UEVENT(0x04a3, 0xf), C_UEVENT(0x05a3, 0xf),
    C_UEVENT(0x06a3, 0xf), C_UEVENT(0x08a3, 0x4), C_UEVENT(0x0ca3, 0x4),
    C_UEVENT(0x01c0, 0x2), C_EVENT(0xd0, 0xf),    C_EVENT(0xd1, 0xf),
    C_EVENT(0xd2, 0xf),    C_EVENT(0xd3, 0xf),    C_END};
static const struct constraint c_gen[] = {C_FIXED(0x00c0, 0), C_FIXED(0x003c, 1),
                                          C_FIXED(0x0300, 2), C_END};
static const struct constraint c_grt[] = {C_FIXED(0x00c0, 0), C_FIXED(0x003c, 1),
                                          C_FIXED(0x0300, 2), C_FIXED(0x013c, 2), C_END};
static const struct constraint c_skl[] = {C_FIXED(0x00c0, 0), C_FIXED(0x003c, 1),
                                          C_FIXED(0x0300, 2), C_UEVENT(0x1c0, 0x2),
                                          C_EVENT(0xd0, 0xf), C_EVENT(0xd1, 0xf),
                                          C_EVENT(0xd2, 0xf), C_EVENT(0xcd, 0xf),
                                          C_EVENT(0xc6, 0xf), C_END};
static const struct constraint c_icl[] = {C_FIXED(0x00c0, 0),
                                          C_FIXED(0x01c0, 0),
                                          C_FIXED(0x0100, 0),
                                          C_FIXED(0x003c, 1),
                                          C_FIXED(0x0300, 2),
                                          C_FIXED(0x0400, 3),
                                          C_METRIC(0x8000),
                                          C_METRIC(0x8100),
                                          C_METRIC(0x8200),
                                          C_METRIC(0x8300),
                                          C_RANGE(0x03, 0x0a, 0xf),
                                          C_RANGE(0x1f, 0x28, 0xf),
                                          C_EVENT(0x32, 0xf),
                                          C_RANGE(0x48, 0x56, 0xf),
                                          C_RANGE(0x60, 0x8b, 0xf),
                                          C_UEVENT(0x04a3, 0xff),
                                          C_UEVENT(0x10a3, 0xff),
                                          C_UEVENT(0x14a3, 0xff),
                                          C_EVENT(0xa3, 0xf),
                                          C_RANGE(0xa8, 0xb0, 0xf),
                                          C_RANGE(0xb7, 0xbd, 0xf),
                                          C_RANGE(0xd0, 0xe6, 0xf),
                                          C_EVENT(0xef, 0xf),
                                          C_RANGE(0xf0, 0xf4, 0xf),
                                          C_END};
static const struct constraint c_glc[] = {
    C_FIXED(0x00c0, 0),       C_FIXED(0x0100, 0),        C_FIXED(0x003c, 1),
    C_FIXED(0x0300, 2),       C_FIXED(0x013c, 2),        C_FIXED(0x0400, 3),
    C_METRIC(0x8000),         C_METRIC(0x8100),          C_METRIC(0x8200),
    C_METRIC(0x8300),         C_METRIC(0x8400),          C_METRIC(0x8500),
    C_METRIC(0x8600),         C_METRIC(0x8700),          C_EVENT(0x2e, 0xff),
    C_EVENT(0x3c, 0xff),      C_RANGE(0x01, 0x8f, 0xf),  C_UEVENT(0x01a3, 0xf),
    C_UEVENT(0x02a3, 0xf),    C_UEVENT(0x08a3, 0xf),     C_UEVENT(0x04a4, 0x1),
    C_UEVENT(0x08a4, 0x1),    C_UEVENT(0x02cd, 0x1),     C_EVENT(0xce, 0x1),
    C_RANGE(0xd0, 0xdf, 0xf), C_RANGE(0x90, 0xfe, 0xff), C_END};
static const struct constraint c_hsw[] = {C_FIXED(0x00c0, 0),    C_FIXED(0x003c, 1),
                                          C_FIXED(0x0300, 2),    C_UEVENT(0x148, 0x4),
                                          C_UEVENT(0x01c0, 0x2), C_EVENT(0xcd, 0x8),
                                          C_UEVENT(0x08a3, 0x4), C_UEVENT(0x0ca3, 0x4),
                                          C_UEVENT(0x04a3, 0xf), C_EVENT(0xd0, 0xf),
                                          C_EVENT(0xd1, 0xf),    C_EVENT(0xd2, 0xf),
                                          C_EVENT(0xd3, 0xf),    C_END};
static const struct constraint c_bdw[] = {C_FIXED(0x00c0, 0), C_FIXED(0x003c, 1),
                                          C_FIXED(0x0300, 2), C_UEVENT(0x148, 0x4),
                                          C_UBIT(0x8a3, 0x4), C_EVENT(0xd0, 0xf),
                                          C_EVENT(0xd1, 0xf), C_EVENT(0xd2, 0xf),
                                          C_EVENT(0xcd, 0xf), C_END};

/* Intel's models, family 6, and their tables, as `intel_pmu_init` sets them
 * (arch/x86/events/intel/core.c:6591-7290); `cp` is Haswell's and its
 * successors' rule that a TSX-checkpointed event counts only on counter 2
 * (`hsw_get_event_constraints`, :4522-4538). Another model with version 2
 * to 4 has the generic table, and one with version 1 none. */
static const struct {
    uint8_t model, cp;
    const struct constraint *table;
} intel_tables[] = {
    {0x0E, 0, c_core},     {0x0F, 0, c_core2},    {0x16, 0, c_core2},   {0x17, 0, c_core2},
    {0x1D, 0, c_core2},    {0x1E, 0, c_nehalem},  {0x1A, 0, c_nehalem}, {0x2E, 0, c_nehalem},
    {0x1C, 0, c_gen},      {0x26, 0, c_gen},      {0x36, 0, c_gen},     {0x27, 0, c_gen},
    {0x35, 0, c_gen},      {0x37, 0, c_gen},      {0x4D, 0, c_gen},     {0x4A, 0, c_gen},
    {0x4C, 0, c_gen},      {0x5A, 0, c_gen},      {0x5C, 0, c_gen},     {0x5F, 0, c_gen},
    {0x7A, 0, c_gen},      {0x86, 0, c_gen},      {0x96, 0, c_gen},     {0x9C, 0, c_gen},
    {0xBE, 0, c_grt},      {0xB6, 0, c_grt},      {0xAF, 0, c_grt},     {0x25, 0, c_westmere},
    {0x2C, 0, c_westmere}, {0x2F, 0, c_westmere}, {0x2A, 0, c_snb},     {0x2D, 0, c_snb},
    {0x3A, 0, c_ivb},      {0x3E, 0, c_ivb},      {0x3C, 1, c_hsw},     {0x3F, 1, c_hsw},
    {0x45, 1, c_hsw},      {0x46, 1, c_hsw},      {0x3D, 1, c_bdw},     {0x56, 1, c_bdw},
    {0x47, 1, c_bdw},      {0x4F, 1, c_bdw},      {0x57, 0, c_gen},     {0x85, 0, c_gen},
    {0x55, 1, c_skl},      {0x4E, 1, c_skl},      {0x5E, 1, c_skl},     {0x8E, 1, c_skl},
    {0x9E, 1, c_skl},      {0xA6, 1, c_skl},      {0xA5, 1, c_skl},     {0x6A, 1, c_icl},
    {0x6C, 1, c_icl},      {0x7E, 1, c_icl},      {0x7D, 1, c_icl},     {0x8C, 1, c_icl},
    {0x8D, 1, c_icl},      {0xA7, 1, c_icl},      {0x8F, 1, c_glc},     {0xCF, 1, c_glc},
    {0xAD, 1, c_glc},      {0xAE, 1, c_glc}};

/* The backend's physical counters. Linux caps the architectural PMU at
 * version 2 and the fixed counters at three; AMD has at most six. `table`
 * is Intel's model's event constraints and `cp` its TSX rule; `family` is
 * AMD's, whose family 15h restricts events to counters and whose family
 * 17h and later merge a counter pair for a large-increment event. The LBR
 * stack is Intel's legacy one, of `lbr_nr` records, with information MSRs
 * when `lbr_info`; `lbr_quirk` is Linux's lbr_from_signext_quirk_wr, and
 * `model` the processor's, which a guest with the stack must have.
 * `fixed0_last` places a guest counter on fixed counter 0 only when no
 * general counter it may use is free: Haswell's fixed counter 0, counting
 * retired instructions with no general counter counting them beside it,
 * overcounts a guest's window now and then. `fw_writes` and `smm_freeze`
 * are PERF_CAPABILITIES' full-width counter writes and FREEZE_WHILE_SMM. */
struct pmu_host {
    int intel, core_ext, pdcm, fw_writes, smm_freeze;
    uint32_t version, nr_gp, nr_fixed, width, fixed_width;
    uint32_t event_length, unavailable;
    const struct constraint *table;
    int cp, fixed0_last;
    uint32_t family;
    uint64_t lbr_format;
    uint32_t lbr_nr, model;
    int lbr_info, lbr_quirk;
};
static struct pmu_host pmu_host;

/* The measuring knob, KVM_PMU_STEP_* of pmu.h; each entry reads it once.
 * `pmu_pmi` is set once an entry has set a physical counter's PMI bit,
 * from when on `kvm_pmu_nmi` reads the global status, and `pmu_placed` is
 * the physical counters the last Intel entry gave its guest's counters,
 * bit n general counter n and bit 32 + j fixed counter j, which the
 * read-only pmu_placed shows. */
static volatile uint32_t pmu_steps;
static volatile int pmu_pmi;
static volatile uint64_t pmu_placed;

/* Header values are copied once, then the event words, so a caller cannot
 * change the allocation's bound between the two copies. */
struct pmu_filter {
    struct kvm_pmu_event_filter head;
    uint64_t event[FILTER_MAX];
};

/** The low n bits, including the empty and full-width masks. */
static uint64_t mask_bits(uint32_t n)
{
    return n >= 64u ? ~0ull : ((1ull << n) - 1u);
}

/** The smaller of two unsigned values. */
static uint32_t minimum(uint32_t a, uint32_t b)
{
    return a < b ? a : b;
}

/** Read a physical PMU MSR, already qualified by the host's model. */
static uint64_t pmu_read(uint32_t index)
{
    uint32_t lo, hi;

    kcpu_rdmsr(index, &lo, &hi);
    return (uint64_t)lo | ((uint64_t)hi << 32);
}

/** Write a physical PMU MSR, with interrupts off and no host PMU user. */
static void pmu_write(uint32_t index, uint64_t value)
{
    kcpu_wrmsr(index, (uint32_t)value, (uint32_t)(value >> 32));
}

/** AMD's physical selector/counter index, using the extended aliases
 * when the host offers them. */
static uint32_t amd_index(uint32_t i, int counter)
{
    if (pmu_host.core_ext) {
        return AMD_CORE_CTL0 + 2u * i + (counter ? 1u : 0u);
    }
    return (counter ? AMD_CTR0 : AMD_CTL0) + i;
}

/** Test an AMD counter without risking a #GP when an outer hypervisor
 * disables its PMU. TCG accepts the MSRs but ignores counter writes. */
static int amd_probe(void)
{
    uint64_t control, counter, value = 0;
    uint32_t flags = kirq_save();
    int result = 0;

    if (kcpu_rdmsr_safe(amd_index(0, 0), &control) != 0 ||
        kcpu_rdmsr_safe(amd_index(0, 1), &counter) != 0 ||
        kcpu_wrmsr_safe(amd_index(0, 0), 0) != 0) {
        goto out;
    }
    if (kcpu_wrmsr_safe(amd_index(0, 1), counter ^ 1u) == 0) {
        result = kcpu_rdmsr_safe(amd_index(0, 1), &value) == 0 && value == (counter ^ 1u);
        (void)kcpu_wrmsr_safe(amd_index(0, 1), counter);
    }
    (void)kcpu_wrmsr_safe(amd_index(0, 0), control);
out:
    kirq_restore(flags);
    return result;
}

/** The model of a CPUID leaf 1 signature, the extended model added from
 * family 6 on, as Linux's x86_model. */
static uint32_t cpu_model(uint32_t signature)
{
    uint32_t family = (signature >> 8) & 0xFu, model = (signature >> 4) & 0xFu;

    if (family == 0xFu) {
        family += (signature >> 20) & 0xFFu;
    }
    if (family >= 6u) {
        model += ((signature >> 16) & 0xFu) << 4;
    }
    return model;
}

/** The value a write of LBR_FROM takes: format 4 without TSX refuses bits
 * 62:61 unless they repeat bits 60:59, which a read leaves clear. */
static uint64_t lbr_from_value(uint64_t value)
{
    if (pmu_host.lbr_quirk) {
        value |= (value & (3ull << 59)) << 2;
    }
    return value;
}

/** Whether an LBR MSR reads and takes its value back, so that no MSR list
 * that carries it can fail an entry. */
static int lbr_msr_works(uint32_t index, int from)
{
    uint64_t value;

    return kcpu_rdmsr_safe(index, &value) == 0 &&
           kcpu_wrmsr_safe(index, from ? lbr_from_value(value) : value) == 0;
}

/**
 * Find Intel's legacy LBR stack, which a guest may have from format 4 on,
 * as Linux's vmx_get_perf_capabilities requires the call stack: 16 records
 * on Haswell and Broadwell, and 32 with their information MSRs on Skylake
 * and later, as Linux's intel_pmu_lbr_init_hsw and _skl give them. The
 * architectural LBR's format, 0x3F, is not taken, as Linux's KVM does not.
 */
static void lbr_probe(void)
{
    uint32_t a, b, c, d, i, n, flags;
    uint64_t cap, format;
    int works;

    if (!pmu_host.pdcm || kcpu_rdmsr_safe(PERF_CAP, &cap) != 0) {
        return;
    }
    format = cap & LBR_FORMAT;
    if (format < 4u || format > 7u) {
        return;
    }
    kcpu_id(1, &a, &b, &c, &d);
    pmu_host.model = cpu_model(a);
    kcpu_id_sub(7, 0, &a, &b, &c, &d);
    pmu_host.lbr_quirk = format == 4u && !(b & ((1u << 4) | (1u << 11)));
    n = format == 4u ? 16u : 32u;
    flags = kirq_save();
    works = lbr_msr_works(LBR_SELECT, 0) && lbr_msr_works(LBR_TOS, 0);
    for (i = 0; works && i < n; i++) {
        works = lbr_msr_works(LBR_FROM + i, 1) && lbr_msr_works(LBR_TO + i, 0) &&
                (format == 4u || lbr_msr_works(LBR_INFO + i, 0));
    }
    kirq_restore(flags);
    if (works) {
        pmu_host.lbr_format = format;
        pmu_host.lbr_nr = n;
        pmu_host.lbr_info = format != 4u;
    }
}

/** The family of a CPUID leaf 1 signature, the extended family added to
 * family 15, as Linux's x86_family. */
static uint32_t cpu_family(uint32_t signature)
{
    uint32_t family = (signature >> 8) & 0xFu;

    return family == 0xFu ? family + ((signature >> 20) & 0xFFu) : family;
}

/** Choose Intel's table of event constraints by the processor's model. */
static void intel_table(void)
{
    uint32_t a, b, c, d, i;

    kcpu_id(1, &a, &b, &c, &d);
    pmu_host.table = pmu_host.version > 1u ? c_gen : NULL;
    if (cpu_family(a) != 6u) {
        return;
    }
    for (i = 0; i < sizeof(intel_tables) / sizeof(intel_tables[0]); i++) {
        if (intel_tables[i].model == cpu_model(a)) {
            pmu_host.table = intel_tables[i].table;
            pmu_host.cp = intel_tables[i].cp;
            pmu_host.fixed0_last = pmu_host.table == c_hsw;
        }
    }
}

void kvm_pmu_start(void)
{
    uint32_t a, b, c, d, top;
    uint64_t cap;

    k_memset(&pmu_host, 0, sizeof(pmu_host));
    if (!kvm_backend) {
        return;
    }
    pmu_host.intel = kvm_backend == &kvm_vmx;
    if (pmu_host.intel) {
        kcpu_id(0, &a, &b, &c, &d);
        if (a < 0xAu) {
            return;
        }
        /* Linux gives a hybrid processor's guests no PMU
         * (arch/x86/kvm/pmu.h:188-195). */
        kcpu_id_sub(7, 0, &a, &b, &c, &d);
        if (d & L7_HYBRID) {
            return;
        }
        kcpu_id(0xA, &a, &b, &c, &d);
        if (!(a & 0xFFu) || !((a >> 8) & 0xFFu)) {
            return;
        }
        pmu_host.version = minimum(a & 0xFFu, 2u);
        pmu_host.nr_gp = minimum((a >> 8) & 0xFFu, KVM_PMU_GP_MAX);
        pmu_host.width = minimum((a >> 16) & 0xFFu, 64u);
        pmu_host.event_length = minimum(a >> 24, 7u);
        pmu_host.unavailable = b & (uint32_t)mask_bits(pmu_host.event_length);
        if (pmu_host.version > 1u) {
            pmu_host.nr_fixed = minimum(d & 0x1Fu, KVM_PMU_FIXED_MAX);
            pmu_host.fixed_width = minimum((d >> 5) & 0xFFu, 64u);
        }
        kcpu_id(1, &a, &b, &c, &d);
        pmu_host.pdcm = (c & PDCM) != 0;
        if (pmu_host.pdcm && kcpu_rdmsr_safe(PERF_CAP, &cap) == 0) {
            pmu_host.fw_writes = (cap & FW_WRITES) != 0;
            pmu_host.smm_freeze = (cap & SMM_FREEZE) != 0;
        }
        intel_table();
        if (pmu_host.width) {
            lbr_probe();
        }
    } else {
        kcpu_id(1, &a, &b, &c, &d);
        pmu_host.family = cpu_family(a);
        kcpu_id(0x80000000u, &top, &b, &c, &d);
        kcpu_id(0x80000001u, &a, &b, &c, &d);
        pmu_host.version = 1;
        pmu_host.core_ext = (c & CORE_EXT) != 0;
        pmu_host.nr_gp = pmu_host.core_ext ? 6u : 4u;
        pmu_host.width = 48;
        if (top >= 0x80000022u) {
            kcpu_id(0x80000022u, &a, &b, &c, &d);
            if (a & 1u) {
                pmu_host.version = 2;
                pmu_host.nr_gp = minimum(b & 0xFu, 6u);
            }
        }
        if (!amd_probe()) {
            pmu_host.version = pmu_host.nr_gp = 0;
        }
    }
    if (!pmu_host.width) {
        pmu_host.version = pmu_host.nr_gp = pmu_host.nr_fixed = 0;
    }
    pmu_steps = 0;
    pmu_pmi = 0;
    pmu_placed = 0;
    watchdog_guest_nmi = kvm_pmu_global() ? kvm_pmu_nmi : NULL;
}

/** Write `value` as sysfs shows a number: 0x, its hex digits without
 * leading zeros, and a newline; the length, which `cap` may cut. */
static int32_t show_hex(char *buf, uint32_t cap, uint64_t value)
{
    static const char digits[] = "0123456789abcdef";
    char text[20];
    uint32_t n = 0, shift = 64;

    text[n++] = '0';
    text[n++] = 'x';
    do {
        shift -= 4u;
    } while (shift && !((value >> shift) & 0xFu));
    for (;;) {
        text[n++] = digits[(value >> shift) & 0xFu];
        if (!shift) {
            break;
        }
        shift -= 4u;
    }
    text[n++] = '\n';
    k_memcpy(buf, text, n < cap ? n : cap);
    return (int32_t)n;
}

int32_t kvm_pmu_steps_show(void *obj, int arg, char *buf, uint32_t cap)
{
    static const char digits[] = "0123456789abcdef";
    uint32_t steps = pmu_steps;

    (void)obj;
    (void)arg;
    if (cap >= 5u) {
        buf[0] = '0';
        buf[1] = 'x';
        buf[2] = digits[(steps >> 4) & 0xFu];
        buf[3] = digits[steps & 0xFu];
        buf[4] = '\n';
    }
    return 5;
}

int32_t kvm_pmu_steps_store(void *obj, int arg, const char *buf, uint32_t len)
{
    uint32_t steps = 0, i = 0, n = len, base = 10, digit;

    (void)obj;
    (void)arg;
    /* A number of at most two hex or three decimal digits, then at most one
     * newline: the knob's bits are few. */
    if (n > 0 && buf[n - 1u] == '\n') {
        n--;
    }
    if (n > 2u && buf[0] == '0' && (buf[1] == 'x' || buf[1] == 'X')) {
        base = 16;
        i = 2;
    }
    if (i == n || n - i > (base == 16u ? 2u : 3u)) {
        return -EINVAL;
    }
    for (; i < n; i++) {
        if (buf[i] >= '0' && buf[i] <= '9') {
            digit = (uint32_t)(buf[i] - '0');
        } else if (base == 16u && (buf[i] | 0x20) >= 'a' && (buf[i] | 0x20) <= 'f') {
            digit = (uint32_t)((buf[i] | 0x20) - 'a') + 10u;
        } else {
            return -EINVAL;
        }
        if (digit >= base) {
            return -EINVAL;
        }
        steps = steps * base + digit;
    }
    if ((steps & ~KVM_PMU_STEPS) || ((steps & KVM_PMU_STEP_SMM) && !pmu_host.smm_freeze) ||
        ((steps & KVM_PMU_STEP_PMI) && (!(steps & KVM_PMU_STEP_WRITE) || !kvm_pmu_global()))) {
        return -EINVAL;
    }
    pmu_steps = steps;
    return (int32_t)len;
}

int32_t kvm_pmu_placed_show(void *obj, int arg, char *buf, uint32_t cap)
{
    (void)obj;
    (void)arg;
    return show_hex(buf, cap, pmu_placed);
}

int kvm_pmu_enabled(void)
{
    return pmu_host.version != 0;
}

int kvm_pmu_global(void)
{
    return pmu_host.intel && pmu_host.version > 1u;
}

/** PERF_CAPABILITIES as KVM offers it: full-width writes and the host's
 * LBR format, as Linux's vmx_get_perf_capabilities. */
static uint64_t supported_caps(void)
{
    return pmu_host.intel && kvm_pmu_enabled() ? FW_WRITES | pmu_host.lbr_format : 0;
}

void kvm_pmu_cpuid(struct kvm_cpuid_entry2 *e)
{
    int enabled = kvm_pmu_enabled();

    switch (e->function) {
    case 1:
        e->ecx &= ~PDCM;
        if (enabled && pmu_host.pdcm) {
            e->ecx |= PDCM;
        }
        break;
    case 0xA:
        e->eax = e->ebx = e->ecx = e->edx = 0;
        if (enabled && pmu_host.intel) {
            e->eax = pmu_host.version | (pmu_host.nr_gp << 8) | (pmu_host.width << 16) |
                     (pmu_host.event_length << 24);
            e->ebx = pmu_host.unavailable;
            e->edx = pmu_host.nr_fixed | (pmu_host.fixed_width << 5);
        }
        break;
    case 0x80000001u:
        e->ecx &= ~CORE_EXT;
        if (enabled && pmu_host.core_ext) {
            e->ecx |= CORE_EXT;
        }
        break;
    case 0x80000022u:
        e->eax = e->ebx = e->ecx = e->edx = 0;
        if (enabled && !pmu_host.intel && pmu_host.version > 1u) {
            e->eax = 1;
            e->ebx = pmu_host.nr_gp;
        }
        break;
    default:
        break;
    }
}

/** The guest's valid GLOBAL_CTRL bits. */
static uint64_t global_mask(const struct kvm_pmu *p)
{
    return mask_bits(p->nr_gp) | (mask_bits(p->nr_fixed) << 32);
}

/** GLOBAL_STATUS and OVF_CTRL additionally have Intel's buffer overflow
 * and condition-changed flags, as Linux's global_status_rsvd allows. */
static uint64_t status_mask(const struct kvm_pmu *p)
{
    return global_mask(p) | (pmu_host.intel ? (3ull << 62) : 0);
}

/** Whether the guest has PERF_CAPABILITIES through CPUID.PDCM. */
static int guest_pdcm(const struct kvm_vcpu *v)
{
    const struct kvm_cpuid_entry2 *e = kvm_cpuid_find(v, 1, 0);

    return e && (e->ecx & PDCM);
}

/** Whether the guest's CPUID model is the host's, which the LBR stack's
 * format belongs to, as Linux's cpuid_model_is_consistent. */
static int guest_model(const struct kvm_vcpu *v)
{
    const struct kvm_cpuid_entry2 *e = kvm_cpuid_find(v, 1, 0);

    return e && cpu_model(e->eax) == pmu_host.model;
}

void kvm_pmu_reset(struct kvm_vcpu *v)
{
    struct kvm_pmu *p = &v->pmu;

    k_memset(p->counter, 0, sizeof(p->counter));
    k_memset(p->eventsel, 0, sizeof(p->eventsel));
    k_memset(p->fixed, 0, sizeof(p->fixed));
    p->fixed_ctrl = p->global_status = 0;
    p->global_ctrl = 0;
    p->run_gp = p->run_fixed = 0;
    p->pmi = 0;
}

void kvm_pmu_refresh(struct kvm_vcpu *v)
{
    struct kvm_pmu *p = &v->pmu;
    const struct kvm_cpuid_entry2 *e;

    p->version = p->nr_gp = p->nr_fixed = 0;
    p->gp_mask = p->fixed_mask = 0;
    p->events = 0;
    p->core_ext = 0;
    p->lbr_nr = 0;
    if (!kvm_pmu_enabled() || v->vm->pmu_disabled) {
        kvm_pmu_reset(v);
        return;
    }
    if (pmu_host.intel) {
        e = kvm_cpuid_find(v, 0xA, 0);
        if (e && (e->eax & 0xFFu)) {
            p->version = minimum(e->eax & 0xFFu, pmu_host.version);
            p->nr_gp = minimum((e->eax >> 8) & 0xFFu, pmu_host.nr_gp);
            p->gp_mask = mask_bits(minimum((e->eax >> 16) & 0xFFu, pmu_host.width));
            p->events = ~e->ebx & (uint32_t)mask_bits(minimum(e->eax >> 24, pmu_host.event_length));
            if (p->version > 1u) {
                p->nr_fixed = minimum(e->edx & 0x1Fu, pmu_host.nr_fixed);
                p->fixed_mask = mask_bits(minimum((e->edx >> 5) & 0xFFu, pmu_host.fixed_width));
            }
        }
    } else {
        p->version = 1;
        e = kvm_cpuid_find(v, 0x80000001u, 0);
        p->core_ext = pmu_host.core_ext && e && (e->ecx & CORE_EXT);
        p->nr_gp = p->core_ext ? 6u : 4u;
        e = kvm_cpuid_find(v, 0x80000022u, 0);
        if (e && (e->eax & 1u) && pmu_host.version > 1u) {
            p->version = 2;
            p->nr_gp = e->ebx & 0xFu;
        }
        p->nr_gp = minimum(p->nr_gp, pmu_host.nr_gp);
        p->gp_mask = mask_bits(48);
    }
    /* The stack follows Linux's intel_pmu_refresh: a PMU, the LBR format
     * in PERF_CAPABILITIES, which needs PDCM, and the host's model. */
    if (pmu_host.intel && p->version && pmu_host.lbr_nr && (p->perf_cap & LBR_FORMAT) &&
        guest_pdcm(v) && guest_model(v)) {
        p->lbr_nr = pmu_host.lbr_nr;
    }
    kvm_pmu_reset(v);
    if (p->version > 1u) {
        p->global_ctrl = mask_bits(p->nr_gp);
    }
}

void kvm_pmu_init(struct kvm_vcpu *v)
{
    v->pmu.perf_cap = supported_caps();
    v->pmu.lbr_list = ~0u;
    kvm_pmu_refresh(v);
}

uint32_t kvm_pmu_msr_list(uint32_t *out)
{
    uint32_t n = 0, i;

    if (!kvm_pmu_enabled()) {
        return 0;
    }
    for (i = 0; i < pmu_host.nr_gp; i++) {
        out[n++] = pmu_host.intel ? EVENTSEL0 + i : amd_index(i, 0);
        out[n++] = pmu_host.intel ? PMC0 + i : amd_index(i, 1);
    }
    if (pmu_host.intel) {
        out[n++] = PERF_CAP;
        for (i = 0; i < pmu_host.nr_fixed; i++) {
            out[n++] = FIXED0 + i;
        }
        if (pmu_host.version > 1u) {
            out[n++] = FIXED_CTRL;
        }
    }
    if (pmu_host.version > 1u) {
        out[n++] = pmu_host.intel ? GLOBAL_CTRL : AMD_GLOBAL;
        out[n++] = pmu_host.intel ? GLOBAL_STATUS : AMD_STATUS;
        out[n++] = pmu_host.intel ? GLOBAL_OVF : AMD_CLEAR;
    }
    return n;
}

int kvm_pmu_feature(uint32_t index, uint64_t *value)
{
    if (index != PERF_CAP) {
        return -1;
    }
    *value = supported_caps();
    return 0;
}

/** Find a general counter MSR or selector, returning its index or -1.
 * full identifies Intel's full-width aliases, ctl its event selectors. */
static int gp_index(struct kvm_vcpu *v, uint32_t msr, int *ctl, int *full)
{
    struct kvm_pmu *p = &v->pmu;
    uint32_t i;

    *full = *ctl = 0;
    if (!p->version) {
        return -1;
    }
    if (pmu_host.intel) {
        if (msr >= PMC0 && msr < PMC0 + p->nr_gp) {
            return (int)(msr - PMC0);
        }
        if (msr >= FULL_PMC0 && msr < FULL_PMC0 + p->nr_gp && guest_pdcm(v) &&
            (p->perf_cap & FW_WRITES)) {
            *full = 1;
            return (int)(msr - FULL_PMC0);
        }
        if (msr >= EVENTSEL0 && msr < EVENTSEL0 + p->nr_gp) {
            *ctl = 1;
            return (int)(msr - EVENTSEL0);
        }
    } else {
        if (msr >= AMD_CTL0 && msr < AMD_CTL0 + 4u) {
            *ctl = 1;
            i = msr - AMD_CTL0;
        } else if (msr >= AMD_CTR0 && msr < AMD_CTR0 + 4u) {
            i = msr - AMD_CTR0;
        } else if (p->core_ext && msr >= AMD_CORE_CTL0 && msr < AMD_CORE_CTL0 + 2u * p->nr_gp) {
            *ctl = !(msr & 1u);
            i = (msr - AMD_CORE_CTL0) / 2u;
        } else {
            return -1;
        }
        return i < p->nr_gp ? (int)i : -1;
    }
    return -1;
}

/** Intel's selector bits KVM accepts, as Linux's reserved_bits. TSX's
 * two flags require the guest's HLE/RTM and the checkpoint flag counter 2. */
static uint64_t intel_selector_mask(struct kvm_vcpu *v, int i)
{
    const struct kvm_cpuid_entry2 *e = kvm_cpuid_find(v, 7, 0);
    uint32_t a, b, c, d;
    uint64_t mask = 0xFFDFFFFFull;

    kcpu_id_sub(7, 0, &a, &b, &c, &d);
    if (e && (e->ebx & b & ((1u << 4) | (1u << 11)))) {
        mask |= EV_TX;
        if (i == 2) {
            mask |= EV_TXCP;
        }
    }
    return mask;
}

/** Read or write one counter, truncating to its architectural width. */
static void counter_io(uint64_t *counter, uint64_t mask, int write, uint64_t *value)
{
    if (write) {
        *counter = *value & mask;
    } else {
        *value = *counter & mask;
    }
}

/** Access PERF_CAPABILITIES, writable only by the caller before a run,
 * with a PMU, and with no LBR format but the host's on the host's model,
 * as Linux's vmx_set_msr. */
static int capabilities_io(struct kvm_vcpu *v, int write, uint64_t *value, int host)
{
    struct kvm_pmu *p = &v->pmu;
    uint64_t format;

    if (!host && (!guest_pdcm(v) || write)) {
        return -1;
    }
    if (!write) {
        *value = p->perf_cap;
    } else {
        format = *value & LBR_FORMAT;
        if ((*value & ~supported_caps()) || (v->has_run && *value != p->perf_cap) ||
            (*value && !p->version) ||
            (format && (format != pmu_host.lbr_format || !guest_model(v)))) {
            return -1;
        }
        if (p->perf_cap != *value) {
            p->perf_cap = *value;
            kvm_pmu_refresh(v);
        }
    }
    return 0;
}

/** Access a general counter or selector; 1 when index is neither. */
static int general_io(struct kvm_vcpu *v, uint32_t index, int write, uint64_t *value, int host)
{
    struct kvm_pmu *p = &v->pmu;
    uint64_t data = *value;
    int ctl, full, i = gp_index(v, index, &ctl, &full);

    if (i < 0) {
        return 1;
    }
    if (ctl) {
        if (!write) {
            *value = p->eventsel[i];
            return 0;
        }
        if (pmu_host.intel) {
            if (data & ~intel_selector_mask(v, i)) {
                return -1;
            }
        } else {
            data &= ~0xFFFFFFF000280000ull;
        }
        p->eventsel[i] = data;
    } else {
        if (write && pmu_host.intel) {
            if (full && (data & ~p->gp_mask)) {
                return -1;
            }
            if (!host && !full) {
                data = (uint64_t)(int64_t)(int32_t)data;
            }
        }
        counter_io(&p->counter[i], p->gp_mask, write, &data);
        if (!write) {
            *value = data;
        }
    }
    return 0;
}

/** Access the version-two global registers; 1 for another MSR. */
static int global_io(struct kvm_pmu *p, uint32_t index, int write, uint64_t *value, int host)
{
    uint64_t mask;

    if (index == (pmu_host.intel ? GLOBAL_CTRL : AMD_GLOBAL)) {
        if (!write) {
            *value = p->global_ctrl;
        } else {
            mask = global_mask(p);
            if (pmu_host.intel && (*value & ~mask)) {
                return -1;
            }
            p->global_ctrl = *value & mask;
        }
        return 0;
    }
    if (index == (pmu_host.intel ? GLOBAL_STATUS : AMD_STATUS)) {
        if (!write) {
            *value = p->global_status;
        } else if (host) {
            if (*value & ~status_mask(p)) {
                return -1;
            }
            p->global_status = *value;
        } else if (pmu_host.intel) {
            return -1;
        }
        return 0;
    }
    if (index == (pmu_host.intel ? GLOBAL_OVF : AMD_CLEAR)) {
        if (!write) {
            *value = 0;
        } else {
            if (pmu_host.intel && (*value & ~status_mask(p))) {
                return -1;
            }
            if (!host) {
                p->global_status &= ~*value;
            }
        }
        return 0;
    }
    return 1;
}

/** Access Intel's fixed counters and their control; 1 for another MSR. */
static int fixed_io(struct kvm_pmu *p, uint32_t index, int write, uint64_t *value)
{
    uint64_t mask = 0;
    uint32_t i;

    if (index >= FIXED0 && index < FIXED0 + p->nr_fixed) {
        counter_io(&p->fixed[index - FIXED0], p->fixed_mask, write, value);
        return 0;
    }
    if (index != FIXED_CTRL || p->version < 2u) {
        return 1;
    }
    if (!write) {
        *value = p->fixed_ctrl;
    } else {
        for (i = 0; i < p->nr_fixed; i++) {
            mask |= 0xBull << (4u * i);
        }
        if (*value & ~mask) {
            return -1;
        }
        p->fixed_ctrl = *value;
    }
    return 0;
}

/**
 * Access the guest's LBR stack, held as the last exit stored it; 1 for
 * another MSR. A write is tried on the processor's register first, which
 * holds no host state and which the next entry with the stack loads again,
 * so that it faults where the guest's own WRMSR would under Linux, whose
 * guest writes the registers themselves. The caller's write of an LBR_FROM
 * value a read returned is tried as the entry loads it, with its sign
 * bits, since Linux's host-initiated write does not fail
 * (arch/x86/kvm/vmx/pmu_intel.c:264-296).
 */
static int lbr_io(struct kvm_pmu *p, uint32_t index, int write, uint64_t *value, int host)
{
    uint64_t *slot, tried;
    uint32_t flags;
    int r;

    if (!p->lbr_nr) {
        return 1;
    }
    if (index == LBR_SELECT) {
        slot = &p->lbr_select;
    } else if (index == LBR_TOS) {
        slot = &p->lbr_tos;
    } else if (index - LBR_FROM < p->lbr_nr) {
        slot = &p->lbr_from[index - LBR_FROM];
    } else if (index - LBR_TO < p->lbr_nr) {
        slot = &p->lbr_to[index - LBR_TO];
    } else if (pmu_host.lbr_info && index - LBR_INFO < p->lbr_nr) {
        slot = &p->lbr_info[index - LBR_INFO];
    } else {
        return 1;
    }
    if (!write) {
        *value = *slot;
        return 0;
    }
    tried = host && index - LBR_FROM < p->lbr_nr ? lbr_from_value(*value) : *value;
    flags = kirq_save();
    r = kcpu_wrmsr_safe(index, tried);
    kirq_restore(flags);
    if (r != 0) {
        return -1;
    }
    *slot = *value;
    return 0;
}

int kvm_pmu_debugctl(struct kvm_vcpu *v, int write, uint64_t *value, int host)
{
    const struct kvm_cpuid_entry2 *e = kvm_cpuid_find(v, 7, 0);
    struct kvm_pmu *p = &v->pmu;
    uint32_t a, b = 0, c = 0, d;
    uint64_t allowed = 0;

    if (!write) {
        *value = p->debugctl;
        return 0;
    }
    /* Linux's vmx_get_supported_debugctl (arch/x86/kvm/vmx/vmx.c:2195-2213):
     * bus-lock detection and RTM debugging where the host has them and the
     * guest's CPUID offers them, or for the caller. */
    kcpu_id(0, &a, &b, &c, &d);
    if (a >= 7u) {
        kcpu_id_sub(7, 0, &a, &b, &c, &d);
    } else {
        b = c = 0;
    }
    if ((c & L7_BLD) && (host || (e && (e->ecx & L7_BLD)))) {
        allowed |= DEBUGCTL_BLD;
    }
    if (pmu_host.lbr_nr && (host || p->lbr_nr)) {
        allowed |= DEBUGCTL_LBR | DEBUGCTL_FRZ;
    }
    if ((b & L7_RTM) && (host || (e && (e->ebx & L7_RTM)))) {
        allowed |= DEBUGCTL_RTM;
    }
    if (*value & ~(allowed | DEBUGCTL_LBR | DEBUGCTL_BTF)) {
        return -1;
    }
    p->debugctl = *value & allowed;
    return 0;
}

uint64_t kvm_pmu_vmcs_debugctl(const struct kvm_vcpu *v)
{
    const struct kvm_pmu *p = &v->pmu;
    uint64_t value = p->lbr_nr ? p->debugctl : p->debugctl & ~(DEBUGCTL_LBR | DEBUGCTL_FRZ);

    /* Linux keeps the host's FREEZE_WHILE_SMM, which its perf sets by
     * default, in the guest's (arch/x86/kvm/vmx/vmx.h:441-462,
     * arch/x86/events/intel/core.c:5153-5154 and 5463). */
    return (p->steps & KVM_PMU_STEP_SMM) ? value | KVM_DEBUGCTL_FREEZE_IN_SMM : value;
}

int kvm_pmu_msr(struct kvm_vcpu *v, uint32_t index, int write, uint64_t *value, int host)
{
    struct kvm_pmu *p = &v->pmu;
    int r;

    if (index == PERF_CAP) {
        return capabilities_io(v, write, value, host);
    }
    if (index == DEBUGCTL && pmu_host.intel) {
        return kvm_pmu_debugctl(v, write, value, host);
    }
    r = lbr_io(p, index, write, value, host);
    if (r != 1) {
        return r;
    }
    r = general_io(v, index, write, value, host);
    if (r != 1) {
        return r;
    }
    if (pmu_host.intel && p->version) {
        r = fixed_io(p, index, write, value);
        if (r != 1) {
            return r;
        }
    }
    if (p->version > 1u) {
        r = global_io(p, index, write, value, host);
        if (r != 1) {
            return r;
        }
    }
    /* Linux keeps the legacy P6 and K7 MSRs as zero without their PMU. */
    if (index == PMC0 || index == PMC0 + 1u || index == EVENTSEL0 || index == EVENTSEL0 + 1u ||
        (index >= AMD_CTL0 && index < AMD_CTR0 + 4u)) {
        if (!write) {
            *value = 0;
        }
        return 0;
    }
    return 1;
}

int kvm_pmu_rdpmc(struct kvm_vcpu *v)
{
    struct kvm_pmu *p = &v->pmu;
    uint32_t index = (uint32_t)v->regs.rcx, type = index & 0xFFFF0000u;
    uint64_t value;

    if (!p->version || ((v->sregs.cr0 & 1u) && !(v->sregs.cr4 & 0x100u) &&
                        ((v->regs.rflags & 0x20000u) || (v->sregs.cs.selector & 3u)))) {
        goto fault;
    }
    if (pmu_host.intel && type == (1u << 30)) {
        index &= 0xFFFFu;
        if (index >= p->nr_fixed) {
            goto fault;
        }
        value = p->fixed[index] & p->fixed_mask;
    } else {
        if ((pmu_host.intel && type) || index >= p->nr_gp) {
            goto fault;
        }
        value = p->counter[index] & p->gp_mask;
    }
    v->regs.rax = (uint32_t)value;
    v->regs.rdx = (uint32_t)(value >> 32);
    return 0;
fault:
    kvm_queue_exception(v, 13, 1, 0);
    return -1;
}

/** Whether one filter word matches the selector, including AMD's high
 * event nibble, under the legacy or masked ABI. */
static int filter_match(const struct pmu_filter *f, uint64_t word, uint64_t selector)
{
    uint64_t events = pmu_host.intel ? 0xFFu : AMD_EVENT;

    if (!(f->head.flags & KVM_PMU_EVENT_FLAG_MASKED_EVENTS)) {
        return word == (selector & (events | 0xFF00u));
    }
    return (word & events) == (selector & events) &&
           (selector & ((word >> 48) & 0xFF00u)) == (word & 0xFF00u);
}

/** Whether the VM's filter permits this general or fixed counter. */
static int allowed(struct kvm_vcpu *v, uint32_t i, int fixed)
{
    const struct pmu_filter *f = v->vm->pmu_filter;
    uint32_t n;
    int include = 0, exclude = 0, match;

    if (!f) {
        return 1;
    }
    if (fixed) {
        match = (f->head.fixed_counter_bitmap & (1u << i)) != 0;
    } else {
        for (n = 0; n < f->head.nevents; n++) {
            if (!filter_match(f, f->event[n], v->pmu.eventsel[i])) {
                continue;
            }
            if ((f->head.flags & KVM_PMU_EVENT_FLAG_MASKED_EVENTS) &&
                (f->event[n] & FILTER_EXCLUDE)) {
                exclude = 1;
            } else {
                include = 1;
            }
        }
        match = include && !exclude;
    }
    return f->head.action == KVM_PMU_EVENT_ALLOW ? match : !match;
}

int32_t kvm_pmu_filter(struct kvm_vm *vm, uintptr_t arg)
{
    struct kvm_pmu_event_filter head;
    struct pmu_filter *f, *old;
    uint64_t valid = (pmu_host.intel ? 0xFFu : AMD_EVENT) | 0xFF00u | FILTER_MASK | FILTER_EXCLUDE;
    uint32_t i;

    if (copy_from_user(&head, (const void __user *)arg, sizeof(head)) != 0) {
        return -EFAULT;
    }
    if (head.action > KVM_PMU_EVENT_DENY || (head.flags & ~KVM_PMU_EVENT_FLAG_MASKED_EVENTS)) {
        return -EINVAL;
    }
    if (head.nevents > FILTER_MAX) {
        return -E2BIG;
    }
    f = kmalloc(sizeof(*f));
    if (!f) {
        return -ENOMEM;
    }
    f->head = head;
    if (head.nevents && copy_from_user(f->event, (const void __user *)(arg + sizeof(head)),
                                       head.nevents * sizeof(uint64_t)) != 0) {
        kfree(f);
        return -EFAULT;
    }
    if (head.flags & KVM_PMU_EVENT_FLAG_MASKED_EVENTS) {
        for (i = 0; i < head.nevents; i++) {
            if (f->event[i] & ~valid) {
                kfree(f);
                return -EINVAL;
            }
        }
    }
    old = vm->pmu_filter;
    vm->pmu_filter = f;
    for (i = 0; i < vm->nr_vcpus; i++) {
        kvm_vcpu_kick(vm->vcpus[i], KVM_REQ_PMU);
    }
    kfree(old);
    return 0;
}

int32_t kvm_pmu_enable(struct kvm_vm *vm, const struct kvm_enable_cap *cap)
{
    if (cap->flags || (cap->args[0] & ~(uint64_t)KVM_PMU_CAP_DISABLE) || vm->nr_vcpus) {
        return -EINVAL;
    }
    vm->pmu_disabled = (cap->args[0] & KVM_PMU_CAP_DISABLE) != 0;
    return 0;
}

void kvm_pmu_vm_free(struct kvm_vm *vm)
{
    kfree(vm->pmu_filter);
    vm->pmu_filter = NULL;
}

/** Whether a general event is enabled by its selector and global bit. */
static int gp_enabled(const struct kvm_pmu *p, uint32_t i)
{
    return (p->eventsel[i] & EV_ENABLE) && (p->version < 2u || (p->global_ctrl & (1ull << i)));
}

/** Whether a fixed event is enabled by its control field and global bit. */
static int fixed_enabled(const struct kvm_pmu *p, uint32_t i)
{
    return (p->fixed_ctrl & (3ull << (4u * i))) && (p->global_ctrl & (1ull << (32u + i)));
}

void kvm_pmu_prepare(struct kvm_vcpu *v)
{
    struct kvm_pmu *p = &v->pmu;
    uint32_t i;

    (void)kvm_vcpu_test_clear(v, KVM_REQ_PMU);
    if (p->pmi) {
        p->pmi = 0;
        /* Versions 2 and 3 freeze the stack on a PMI by clearing
         * DEBUGCTL.LBR, as Linux's intel_pmu_legacy_freezing_lbrs_on_pmi. */
        if (p->lbr_nr && p->version > 1u && p->version < 4u && (p->debugctl & DEBUGCTL_FRZ)) {
            p->debugctl &= ~DEBUGCTL_LBR;
        }
        kvm_lapic_pmi(v);
    }
    p->run_gp = p->run_fixed = 0;
    for (i = 0; i < p->nr_gp; i++) {
        if (gp_enabled(p, i) && allowed(v, i, 0)) {
            p->run_gp |= 1u << i;
        }
    }
    for (i = 0; i < p->nr_fixed; i++) {
        if (fixed_enabled(p, i) && allowed(v, i, 1)) {
            p->run_fixed |= 1u << i;
        }
    }
}

/** Add a measured delta, recording a wrap even when its interrupt is
 * disabled. A delta is the difference of two physical counts, which the
 * physical width bounds; no run comes near it. */
static void accumulate(struct kvm_pmu *p, uint32_t i, int fixed, uint64_t delta)
{
    uint64_t *counter = fixed ? &p->fixed[i] : &p->counter[i];
    uint64_t mask = fixed ? p->fixed_mask : p->gp_mask;
    uint64_t interrupt = fixed ? p->fixed_ctrl & (8ull << (4u * i)) : p->eventsel[i] & EV_INT;

    if (delta > mask - *counter) {
        p->global_status |= 1ull << (fixed ? 32u + i : i);
        if (interrupt) {
            p->pmi = 1;
        }
    }
    *counter = (*counter + delta) & mask;
}

void kvm_pmu_retired(struct kvm_vcpu *v)
{
    struct kvm_pmu *p = &v->pmu;
    uint64_t select, event_mask = pmu_host.intel ? 0xFFu : AMD_EVENT;
    uint64_t mode = ((v->regs.rflags & 0x20000u) || (v->sregs.cs.selector & 3u)) ? EV_USR : EV_OS;
    uint32_t i;

    for (i = 0; i < p->nr_gp; i++) {
        select = p->eventsel[i];
        if ((select & (event_mask | 0xFF00u)) == 0xC0u && !(select & (EV_TX | EV_TXCP)) &&
            (select & mode) && gp_enabled(p, i) && allowed(v, i, 0)) {
            accumulate(p, i, 0, 1);
        }
    }
    if (p->nr_fixed && fixed_enabled(p, 0) && allowed(v, 0, 1) &&
        (p->fixed_ctrl & (mode == EV_USR ? 2u : 1u))) {
        accumulate(p, 0, 1, 1);
    }
}

/** The number of bits set in `m`. */
static uint32_t weight(uint64_t m)
{
    uint32_t n = 0;

    for (; m; m &= m - 1u) {
        n++;
    }
    return n;
}

/** The physical counters, general and fixed, of the host's PMU. */
static uint64_t all_counters(void)
{
    return mask_bits(pmu_host.nr_gp) | (mask_bits(pmu_host.nr_fixed) << 32);
}

/**
 * The physical counters Intel's event `select` may run on, as Linux's
 * `x86_get_event_constraints` (arch/x86/events/intel/core.c:3425-3442)
 * reads the model's table, with a fixed row widened to the general
 * counters unless its code is a pseudo-encoding
 * (arch/x86/events/intel/core.c:6343-6361), and the TSX-checkpoint rule of
 * `hsw_get_event_constraints`.
 */
static uint64_t intel_constraint(uint64_t select)
{
    const struct constraint *c;
    uint64_t mask = mask_bits(pmu_host.nr_gp);

    for (c = pmu_host.table; c && c->match; c++) {
        if ((select & c->match) - c->code <= c->size) {
            mask = c->mask;
            if (c->match == M_FIXED && (c->code & 0xFFu)) {
                mask |= mask_bits(pmu_host.nr_gp);
            }
            break;
        }
    }
    if (pmu_host.cp && (select & EV_TXCP)) {
        mask &= 1ull << 2;
    }
    return mask & all_counters();
}

/** The number of unit-mask bits AMD's `select` sets. */
static uint32_t amd_umask_bits(uint64_t select)
{
    return weight((select >> 8) & 0xFFu);
}

/**
 * The counters AMD's event `select` may run on: family 15h's by Linux's
 * `amd_get_event_constraints_f15h` (arch/x86/events/amd/core.c:1161-1229),
 * and any counter on the others. A pair event of family 17h and later is
 * `amd_pair`'s.
 */
static uint64_t amd_constraint(uint64_t select)
{
    uint32_t code = (uint32_t)(((select >> 24) & 0xF00u) | (select & 0xFFu));
    uint64_t pmc0 = 0x01, pmc20 = 0x07, pmc3 = 0x08, pmc30 = 0x09, pmc50 = 0x3F, pmc53 = 0x38;
    uint64_t mask = mask_bits(pmu_host.nr_gp);

    if (pmu_host.family != 0x15u) {
        return mask;
    }
    switch (code & 0xF0u) {
    case 0x00:
    case 0x10:
        if ((code == 0x000 && (select & 0xF000u) && (select & 0x0F00u)) ||
            (code == 0x004 && amd_umask_bits(select) > 1u) || code == 0x003 || code == 0x00B ||
            code == 0x00D) {
            return mask & pmc3;
        }
        return mask & pmc53;
    case 0x20:
    case 0x30:
    case 0x40:
    case 0x50:
    case 0xC0:
        switch (code) {
        case 0x023:
        case 0x043:
        case 0x045:
        case 0x046:
        case 0x054:
        case 0x055:
            return mask & pmc20;
        case 0x02D:
            return mask & pmc3;
        case 0x02E:
            return mask & pmc30;
        case 0x031:
            return amd_umask_bits(select) <= 1u ? mask & pmc20 : 0;
        case 0x1C0:
            return mask & pmc53;
        default:
            return mask & pmc50;
        }
    case 0x60:
    case 0x70:
    case 0x80:
    case 0x90:
    case 0xD0:
        if (code == 0x08F || code == 0x187 || code == 0x188) {
            return mask & pmc0;
        }
        if ((code >= 0x0DB && code <= 0x0DF) || code == 0x1D6 || code == 0x1D8) {
            return mask & pmc50;
        }
        return mask & pmc20;
    default:
        return 0;
    }
}

/** Whether AMD's `select` is a large-increment event, Retired SSE/AVX
 * FLOPs, which family 17h and later count on an even counter merged with
 * the odd one after it (arch/x86/events/amd/core.c:351-360 and 1474-1484). */
static int amd_pair(uint64_t select)
{
    return pmu_host.family >= 0x17u && (((select >> 24) & 0xF00u) | (select & 0xFFu)) == 0x003u;
}

/** A general counter's physical selector or count. */
static uint32_t gp_msr(uint32_t i, int counter)
{
    if (pmu_host.intel) {
        return (counter ? PMC0 : EVENTSEL0) + i;
    }
    return amd_index(i, counter);
}

/**
 * The counters the host holds on this processor, general ones in the low
 * bits and Intel's fixed ones from bit 32: a general counter whose selector
 * is enabled and a fixed counter whose field of `fixed_ctrl`, FIXED_CTRL,
 * counts, where Intel's global control, `global`, enables them too. KVM
 * clears its own selectors at every exit, and its fixed fields are gated
 * off by the host's global control, so these are the host's, the
 * hard-lockup detector's counter among them.
 */
static uint64_t host_counters(uint64_t global, uint64_t fixed_ctrl)
{
    uint64_t held = 0;
    uint32_t i;

    for (i = 0; i < pmu_host.nr_gp; i++) {
        if (kvm_pmu_global() && !(global & (1ull << i))) {
            continue;
        }
        if (pmu_read(gp_msr(i, 0)) & EV_ENABLE) {
            held |= 1ull << i;
        }
    }
    for (i = 0; kvm_pmu_global() && i < pmu_host.nr_fixed; i++) {
        if ((global & FIXED_BIT(i)) && (fixed_ctrl & (3ull << (4u * i)))) {
            held |= FIXED_BIT(i);
        }
    }
    return held;
}

/** The selector a running guest counter counts by: a general counter's
 * own, without the physical interrupt, or the event of fixed counter j, as
 * Linux's `intel_get_fixed_pmc_eventsel` names it, in the modes its field
 * of FIXED_CTRL gives (arch/x86/kvm/vmx/pmu_intel.c:433-450,
 * arch/x86/kvm/pmu.c:469-478). */
static uint64_t slot_select(const struct kvm_pmu *p, uint32_t s)
{
    static const uint16_t fixed_event[KVM_PMU_FIXED_MAX] = {0x00c0, 0x003c, 0x0300};
    uint64_t field;

    if (s < KVM_PMU_GP_MAX) {
        return p->eventsel[s] & ~(EV_INT | EV_PIN);
    }
    s -= KVM_PMU_GP_MAX;
    field = p->fixed_ctrl >> (4u * s);
    return fixed_event[s] | EV_ENABLE | ((field & 1u) ? EV_OS : 0) | ((field & 2u) ? EV_USR : 0);
}

/** Whether guest slot `s` runs at this entry, by the run masks. */
static int slot_running(const struct kvm_pmu *p, uint32_t s)
{
    if (s < KVM_PMU_GP_MAX) {
        return (p->run_gp & (1u << s)) != 0;
    }
    return (p->run_fixed & (1u << (s - KVM_PMU_GP_MAX))) != 0;
}

/** The counter after `n` in the order of preference: Linux's, fixed
 * counters first and then general ones from 0 (arch/x86/events/core.c:
 * 880-905), but with fixed counter 0 after the general ones where `last`
 * says; 64 after the last, and the first after 64. */
static uint32_t counter_after(uint32_t n, int last)
{
    if (n == 64u) {
        return last ? 33u : 32u;
    }
    if (n == 63u) {
        return 0;
    }
    if (n == 31u) {
        return last ? 32u : 64u;
    }
    if (n == 32u && last) {
        return 64u;
    }
    return n + 1u;
}

/** The next of `want`'s counters after `n` in the order of preference,
 * with fixed counter 0 last where `last` says; 64 after the last. Begin
 * with 64. */
static uint32_t next_counter(uint64_t want, uint32_t n, int last)
{
    for (;;) {
        n = counter_after(n, last);
        if (n == 64u || (want & (1ull << n))) {
            return n;
        }
    }
}

/** Give guest slot `s` one of `want[s]`'s physical counters: a free one
 * if there is one, as Linux's scheduler takes it, and otherwise one whose
 * owner can move to another of its own, an augmenting path of a bipartite
 * matching, which finds a place for every counter whenever one exists.
 * `owner` maps a physical counter to its slot, 0xFF when free; `seen`
 * holds the counters this search has tried; `last` puts fixed counter 0
 * last in the order. */
static int augment(const uint64_t *want, uint8_t *owner, uint8_t *phys, uint32_t s, uint64_t *seen,
                   int last)
{
    uint32_t n;

    for (n = next_counter(want[s], 64u, last); n != 64u; n = next_counter(want[s], n, last)) {
        if (owner[n] == 0xFFu) {
            owner[n] = (uint8_t)s;
            phys[s] = (uint8_t)n;
            return 1;
        }
    }
    for (n = next_counter(want[s], 64u, last); n != 64u; n = next_counter(want[s], n, last)) {
        if (*seen & (1ull << n)) {
            continue;
        }
        *seen |= 1ull << n;
        if (augment(want, owner, phys, owner[n], seen, last)) {
            owner[n] = (uint8_t)s;
            phys[s] = (uint8_t)n;
            return 1;
        }
    }
    return 0;
}

/**
 * Give each running counter of `p` a physical counter in `free` that its
 * event may use, in `p->phys`, the most constrained first, as Linux's
 * scheduler places a vCPU's pinned perf events (arch/x86/events/core.c:
 * 940-1010). An AMD pair event takes an even counter and the odd one after
 * it first, as only family 17h's unconstrained events share its counters.
 * A counter left without one does not count, as a pinned perf event that
 * cannot be scheduled does not, and leaves the run masks. Fixed counter 0
 * comes after the general ones where `fixed0_last` says, unless the knob's
 * KVM_PMU_STEP_ORDER asks for Linux's order. Return the physical counters
 * given.
 */
static uint64_t place(struct kvm_pmu *p, uint64_t free)
{
    uint64_t want[KVM_PMU_SLOTS], seen, used = 0;
    uint8_t owner[32u + KVM_PMU_FIXED_MAX], order[KVM_PMU_SLOTS];
    uint32_t s, k, n = 0, t;
    int last = pmu_host.fixed0_last && !(p->steps & KVM_PMU_STEP_ORDER);

    k_memset(owner, 0xFF, sizeof(owner));
    p->pair = 0;
    for (s = 0; !pmu_host.intel && s < p->nr_gp; s++) {
        if (!(p->run_gp & (1u << s)) || !amd_pair(p->eventsel[s])) {
            continue;
        }
        for (k = 0; k + 1u < pmu_host.nr_gp; k += 2u) {
            if (((free >> k) & 3u) == 3u) {
                free &= ~(3ull << k);
                p->phys[s] = (uint8_t)k;
                p->pair |= 1u << s;
                used |= 3ull << k;
                break;
            }
        }
        if (!(p->pair & (1u << s))) {
            p->run_gp &= ~(1u << s);
        }
    }
    for (s = 0; s < KVM_PMU_SLOTS; s++) {
        want[s] = 0;
        if (!slot_running(p, s) || (p->pair & (1u << s))) {
            continue;
        }
        want[s] = (pmu_host.intel ? intel_constraint(slot_select(p, s))
                                  : amd_constraint(p->eventsel[s])) &
                  free;
        order[n++] = (uint8_t)s;
    }
    /* By weight, stable, as Linux's perf_assign_events orders them. */
    for (s = 1; s < n; s++) {
        for (k = s; k > 0 && weight(want[order[k - 1u]]) > weight(want[order[k]]); k--) {
            t = order[k];
            order[k] = order[k - 1u];
            order[k - 1u] = (uint8_t)t;
        }
    }
    for (k = 0; k < n; k++) {
        seen = 0;
        (void)augment(want, owner, p->phys, order[k], &seen, last);
    }
    for (k = 0; k < n; k++) {
        s = order[k];
        if (owner[p->phys[s]] == s && (want[s] & (1ull << p->phys[s]))) {
            used |= 1ull << p->phys[s];
        } else if (s < KVM_PMU_GP_MAX) {
            p->run_gp &= ~(1u << s);
        } else {
            p->run_fixed &= ~(1u << (s - KVM_PMU_GP_MAX));
        }
    }
    return used;
}

/** Add the count physical counter `n` gained since `base` to slot `s`. */
static void collect(struct kvm_pmu *p, uint32_t s, uint32_t n, uint64_t base)
{
    uint64_t now = n < 32u ? pmu_read(gp_msr(n, 1)) : pmu_read(FIXED0 + n - 32u);
    uint32_t width = n < 32u ? pmu_host.width : pmu_host.fixed_width;

    if (s < KVM_PMU_GP_MAX) {
        accumulate(p, s, 0, (now - base) & mask_bits(width));
    } else {
        accumulate(p, s - KVM_PMU_GP_MAX, 1, (now - base) & mask_bits(width));
    }
}

void kvm_pmu_svm_enter(struct kvm_vcpu *v)
{
    struct kvm_pmu *p = &v->pmu;
    uint32_t i, n;
    uint64_t used;

    if (pmu_host.intel || !p->version || !p->run_gp) {
        return;
    }
    used = place(p, mask_bits(pmu_host.nr_gp) & ~host_counters(0, 0));
    /* Each counter starts as late as it can and stops as early, at the
     * global control when there is one: GuestOnly keeps it from counting
     * the host, unless an outer hypervisor ignores the bit. A pair's odd
     * counter takes the merge before its even one starts, as Linux's
     * `__x86_pmu_enable_event` writes it (perf_event.h:1198-1214). */
    if (pmu_host.version > 1u) {
        p->host_global = pmu_read(AMD_GLOBAL);
        pmu_write(AMD_GLOBAL, p->host_global & ~used);
    }
    for (i = 0; i < p->nr_gp; i++) {
        if (p->run_gp & (1u << i)) {
            n = p->phys[i];
            if (p->pair & (1u << i)) {
                pmu_write(amd_index(n + 1u, 0), AMD_MERGE);
            }
            p->base[i] = pmu_read(amd_index(n, 1));
            pmu_write(amd_index(n, 0), (p->eventsel[i] & ~(EV_INT | EV_PIN)) | EV_GUEST);
        }
    }
    if (pmu_host.version > 1u) {
        pmu_write(AMD_GLOBAL, p->host_global | used);
    }
}

void kvm_pmu_svm_exit(struct kvm_vcpu *v)
{
    struct kvm_pmu *p = &v->pmu;
    uint32_t i, n;
    uint64_t used = 0;

    if (pmu_host.intel || !p->version || !p->run_gp) {
        return;
    }
    for (i = 0; i < p->nr_gp; i++) {
        if (p->run_gp & (1u << i)) {
            used |= ((p->pair & (1u << i)) ? 3ull : 1ull) << p->phys[i];
        }
    }
    if (pmu_host.version > 1u) {
        pmu_write(AMD_GLOBAL, p->host_global & ~used);
    }
    for (i = 0; i < p->nr_gp; i++) {
        if (p->run_gp & (1u << i)) {
            n = p->phys[i];
            pmu_write(amd_index(n, 0), 0);
            if (p->pair & (1u << i)) {
                pmu_write(amd_index(n + 1u, 0), 0);
            }
            collect(p, i, n, p->base[i]);
        }
    }
    if (pmu_host.version > 1u) {
        pmu_write(AMD_GLOBAL, p->host_global);
    }
}

/**
 * The physical count that starts guest slot `s` with Linux's period: the
 * counts left before its guest count wraps, at least 2 and at most perf's
 * max_period, then the model's limit, negated (arch/x86/kvm/pmu.c:167-174
 * and 256-272, arch/x86/events/core.c:1397-1413,
 * arch/x86/events/intel/core.c:7357-7358). The limits are `nhm_limit_period`
 * on Nehalem, at least 32; `hsw_limit_period` on Haswell, at least 32, and
 * 128 for INST_RETIRED.ALL, event 0xC0 with unit mask 1 (erratum HSW11); and
 * `bdw_limit_period` on Broadwell, at least 128 with the low six bits clear
 * for that event alone (arch/x86/events/intel/core.c:4748-4794, chosen at
 * :6629, :6932 and :6975). A fixed counter's event is its own, 0x00C0 for
 * fixed counter 0, as KVM's perf event for it has (`slot_select`).
 */
static uint64_t start_count(const struct kvm_pmu *p, uint32_t s)
{
    uint64_t mask, left, most, least = 0;
    uint32_t width;
    int all = (slot_select(p, s) & 0xFFFFu) == 0x01C0u;

    if (s < KVM_PMU_GP_MAX) {
        mask = p->gp_mask;
        left = (0 - p->counter[s]) & mask;
        width = pmu_host.width;
    } else {
        mask = p->fixed_mask;
        left = (0 - p->fixed[s - KVM_PMU_GP_MAX]) & mask;
        width = pmu_host.fixed_width;
    }
    if (!left) {
        left = mask + 1u;
    }
    most = pmu_host.fw_writes ? mask_bits(pmu_host.width) >> 1 : (1ull << 31) - 1u;
    if (left < 2u) {
        left = 2;
    }
    if (left > most) {
        left = most;
    }
    if (pmu_host.table == c_nehalem || pmu_host.table == c_hsw) {
        least = pmu_host.table == c_hsw && all ? 128u : 32u;
    } else if (pmu_host.table == c_bdw && all) {
        least = 128;
    }
    if (left < least) {
        left = least;
    }
    if (pmu_host.table == c_bdw && all) {
        left &= ~(uint64_t)0x3F;
    }
    return (0 - left) & mask_bits(width);
}

/** Append one VMX MSR entry, including its required zero reserved word. */
static void list_add(struct kvm_pmu_msr *list, uint32_t *n, uint32_t index, uint64_t value)
{
    list[*n].index = index;
    list[*n].reserved = 0;
    list[*n].value = value;
    (*n)++;
}

/**
 * Set each running guest counter's selector or fixed field and read its
 * count. Linux writes no counter at an entry and switches only the global
 * control; the selectors are set here where the host's global control
 * keeps them stopped, or by the lists on a version 1 PMU, and each count is
 * read now and again once the exit has stopped it. The knob's
 * KVM_PMU_STEP_WRITE writes each counter first, as Linux's perf starts an
 * event, and KVM_PMU_STEP_PMI sets the interrupt bits Linux's perf sets
 * (arch/x86/events/intel/core.c:2853-2855). Return the guest's fields of
 * FIXED_CTRL, which the caller writes with the host's.
 */
static uint64_t program_slots(struct kvm_pmu *p, struct kvm_pmu_msr *guest,
                              struct kvm_pmu_msr *host, uint32_t *ng, uint32_t *nh)
{
    uint64_t fixed = 0, select, field;
    uint32_t s, n;

    for (s = 0; s < KVM_PMU_SLOTS; s++) {
        if (!slot_running(p, s)) {
            continue;
        }
        n = p->phys[s];
        select = slot_select(p, s);
        if (p->steps & KVM_PMU_STEP_PMI) {
            select |= EV_INT;
            pmu_pmi = 1;
        }
        if (n >= 32u) {
            n -= 32u;
            field = ((select & EV_OS) ? 1ull : 0) | ((select & EV_USR) ? 2ull : 0);
            fixed |= (field | ((select & EV_INT) ? FIXED_PMI : 0)) << (4u * n);
            if (p->steps & KVM_PMU_STEP_WRITE) {
                pmu_write(FIXED0 + n, start_count(p, s));
            }
            p->base[s] = pmu_read(FIXED0 + n);
            continue;
        }
        if (p->steps & KVM_PMU_STEP_WRITE) {
            pmu_write((pmu_host.fw_writes ? FULL_PMC0 : PMC0) + n, start_count(p, s));
        }
        if (kvm_pmu_global()) {
            pmu_write(EVENTSEL0 + n, select);
        } else {
            list_add(guest, ng, EVENTSEL0 + n, select);
            list_add(host, nh, EVENTSEL0 + n, 0);
        }
        p->base[s] = pmu_read(PMC0 + n);
    }
    return fixed;
}

/** Append the guest's LBR stack to its entry list, which the exit's store
 * list also is, while its DEBUGCTL.LBR is set. */
static void lbr_lists(struct kvm_pmu *p, struct kvm_pmu_msr *guest, uint32_t *ng)
{
    uint32_t i;

    if (!p->lbr_nr || !(p->debugctl & DEBUGCTL_LBR)) {
        return;
    }
    p->lbr_list = *ng;
    list_add(guest, ng, LBR_SELECT, p->lbr_select);
    list_add(guest, ng, LBR_TOS, p->lbr_tos);
    for (i = 0; i < p->lbr_nr; i++) {
        list_add(guest, ng, LBR_FROM + i, lbr_from_value(p->lbr_from[i]));
        list_add(guest, ng, LBR_TO + i, p->lbr_to[i]);
        if (pmu_host.lbr_info) {
            list_add(guest, ng, LBR_INFO + i, p->lbr_info[i]);
        }
    }
}

void kvm_pmu_vmx_enter(struct kvm_vcpu *v, struct kvm_pmu_msr *guest, struct kvm_pmu_msr *host,
                       uint32_t *ng, uint32_t *nh, uint64_t *global)
{
    struct kvm_pmu *p = &v->pmu;
    uint64_t host_global = 0, host_fixed = 0, fixed = 0, held, used = 0, spare;
    uint32_t i;

    *ng = *nh = 0;
    p->lbr_list = ~0u;
    p->steps = pmu_steps;
    if (global) {
        global[0] = global[1] = 0;
    }
    if (!pmu_host.intel || !pmu_host.version) {
        return;
    }
    if (kvm_pmu_global()) {
        host_global = pmu_read(GLOBAL_CTRL);
        if (pmu_host.nr_fixed) {
            host_fixed = pmu_read(FIXED_CTRL);
        }
    }
    held = host_counters(host_global, host_fixed);
    if (p->version) {
        used = place(p, all_counters() & ~held);
    } else {
        p->run_gp = p->run_fixed = 0;
    }
    if (kvm_pmu_global()) {
        /* The counters a guest may have stop in the host, once: the host
         * keeps only the counters it holds enabled, and the fields of the
         * fixed ones it holds. */
        spare = all_counters() & ~held;
        if (host_global & spare) {
            host_global &= ~spare;
            pmu_write(GLOBAL_CTRL, host_global);
        }
        for (i = 0; i < pmu_host.nr_fixed; i++) {
            if (held & FIXED_BIT(i)) {
                fixed |= host_fixed & (0xFull << (4u * i));
            }
        }
    }
    if (used) {
        pmu_placed = used;
    }
    fixed |= program_slots(p, guest, host, ng, nh);
    if (kvm_pmu_global()) {
        if (pmu_host.nr_fixed && fixed != host_fixed) {
            pmu_write(FIXED_CTRL, fixed);
        }
        /* The host's counters count in the guest too, as Linux's do. */
        if (global) {
            global[0] = host_global | used;
            global[1] = host_global;
        } else if (used) {
            list_add(guest, ng, GLOBAL_CTRL, host_global | used);
            list_add(host, nh, GLOBAL_CTRL, host_global);
        }
    }
    lbr_lists(p, guest, ng);
}

void kvm_pmu_vmx_exit(struct kvm_vcpu *v, const struct kvm_pmu_msr *guest, int ran)
{
    struct kvm_pmu *p = &v->pmu;
    uint32_t i, s, k = p->lbr_list;
    uint64_t fields = 0, fixed;

    p->lbr_list = ~0u;
    if (!pmu_host.intel || !pmu_host.version) {
        return;
    }
    for (s = 0; s < KVM_PMU_SLOTS; s++) {
        if (!slot_running(p, s)) {
            continue;
        }
        if (ran) {
            collect(p, s, p->phys[s], p->base[s]);
        }
        if (kvm_pmu_global() && p->phys[s] < 32u) {
            pmu_write(EVENTSEL0 + p->phys[s], 0);
        } else if (p->phys[s] >= 32u) {
            fields |= 0xFull << (4u * (p->phys[s] - 32u));
        }
    }
    /* Linux's perf clears a fixed field when the guest disables its counter
     * (arch/x86/events/intel/core.c:2551); the knob clears it at each exit,
     * and the next entry sets it again. */
    if ((p->steps & KVM_PMU_STEP_FIELD) && kvm_pmu_global() && fields) {
        fixed = pmu_read(FIXED_CTRL);
        if (fixed & fields) {
            pmu_write(FIXED_CTRL, fixed & ~fields);
        }
    }
    if (ran && k != ~0u) {
        p->lbr_select = guest[k++].value;
        p->lbr_tos = guest[k++].value;
        for (i = 0; i < p->lbr_nr; i++) {
            p->lbr_from[i] = guest[k++].value;
            p->lbr_to[i] = guest[k++].value;
            if (pmu_host.lbr_info) {
                p->lbr_info[i] = guest[k++].value;
            }
        }
    }
}

/**
 * Claim an NMI a guest counter raised, as `watchdog_guest_nmi` on a host
 * with Intel's global status: acknowledge the overflow of each counter the
 * host does not hold, which only guests run, in IA32_PERF_GLOBAL_OVF_CTRL,
 * and return how many there were, as Linux's perf handler acknowledges
 * every set bit, counts each as handled (arch/x86/events/intel/
 * core.c:3126-3129 and 3190) and returns the count (events/core.c:
 * 1749-1754), so that a count above one lets the NMI handler swallow the
 * NMI a second overflow latched (kernel/nmi.c:404-405). The guest's
 * overflow itself was counted by the exit (`accumulate`), which delivers
 * its PMI before the next entry.
 * The counter's APIC entry, which the delivery masked, is unmasked by the
 * hard-lockup detector's handler after this while its counter runs, and
 * stays masked otherwise. Only the counters' MSRs are read: an NMI may come
 * at any point of the host's code. 0 until an entry has set a PMI bit.
 */
int kvm_pmu_nmi(void)
{
    uint64_t status, held, fixed = 0, bits;
    int n = 0;

    if (!pmu_pmi || !kvm_pmu_global()) {
        return 0;
    }
    status = pmu_read(GLOBAL_STATUS) & all_counters();
    if (!status) {
        return 0;
    }
    if (pmu_host.nr_fixed) {
        fixed = pmu_read(FIXED_CTRL);
    }
    held = host_counters(pmu_read(GLOBAL_CTRL), fixed);
    status &= ~held;
    if (!status) {
        return 0;
    }
    pmu_write(GLOBAL_OVF, status);
    for (bits = status; bits; bits &= bits - 1u) {
        n++;
    }
    return n;
}
