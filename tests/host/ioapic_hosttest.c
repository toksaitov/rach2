/*
 * ioapic_hosttest.c: ioapic.c's masking of the routed lines and pic.c's of
 * the 8259 pair, which remapping's start and the late x2APIC leave wrap
 * around their moves, the `maxcpus=` parse that keeps the I/O APIC off,
 * and the boot line's `nointremap` and `intremap=` with the DMAR table's
 * flags, which say whether remapping may start in the x2APIC format; and
 * the boot line's lookups of a word, cmdline.c's `kcmdline_has`,
 * `kcmdline_early` and `kcmdline_setup`, which those parses stand on.
 *
 * host-deps: rach2/sys/arch/x86/ioapic.c rach2/sys/arch/x86/pic.c
 * host-deps: rach2/sys/cmdline.c rach2/sys/lib/kstring.c
 * host-deps: rach2/sys/lib/kstd.c
 * host-deps: rach2/sys/lib/kctypes.c tests/host/compat/kirq_none.c
 *
 * The I/O APIC is a model of one chip's registers, by the index its select
 * register takes, reached through `ioapic_set_io`; the 8259 pair is a model
 * of its two mask registers behind `inb` and `outb`. The rows check that a
 * mask and unmask leave every redirection entry as it was, bit for bit,
 * that only the lines that were unmasked come back, and that each edge
 * line among them, and no level one, has its vector raised once by a
 * self-interrupt, for the edge its masked input dropped (`check_irq_resend`
 * in Linux 6.12.115, kernel/irq/resend.c, lines 124 to 153). The parse
 * rows are Linux's get_option over simple_strtoull's base 0 (lib/cmdline.c,
 * lines 56 to 79; lib/kstrtox.c, lines 26 to 40), and its setup_nointremap
 * and setup_irqremap, which every token runs in the line's order and which
 * match each item by its prefix (drivers/iommu/irq_remapping.c, lines 50
 * to 84). Masking before `ioapic_init` touches no chip, since no line is
 * routed yet.
 */

#include "../../rach2/sys/arch/x86/ioapic.h"
#include "../../rach2/sys/arch/x86/pic.h"
#include "../../rach2/sys/cmdline.h"
#include "../../rach2/sys/mm/paging.h"

extern int printf(const char *fmt, ...);

static int failures;

/** Count a failed check and name it. */
static void check(int ok, const char *what)
{
    if (!ok) {
        failures++;
        printf("ioapic_hosttest: fail: %s\n", what);
    }
}

/* ---- the machine around the driver ---- */

/** The log is not read. */
void klog(int level, const char *subsystem, const char *format, ...)
{
    (void)level;
    (void)subsystem;
    (void)format;
}

/* The I/O APIC's registers: id 2, 24 inputs, version 0x20. */
#define CHIP_ID     2
#define CHIP_INPUTS 24
static uint32_t rte[CHIP_INPUTS][2];
static int bad_access;

/** A register of the model, by its select index. */
static uint32_t chip_read(uint8_t id, uint32_t reg)
{
    if (id != CHIP_ID) {
        bad_access++;
        return 0xFFFFFFFFu;
    }
    if (reg == 0x01) {
        return ((uint32_t)(CHIP_INPUTS - 1) << 16) | 0x20u;
    }
    if (reg >= 0x10 && reg < 0x10 + 2 * CHIP_INPUTS) {
        return rte[(reg - 0x10) / 2][(reg - 0x10) % 2];
    }
    bad_access++;
    return 0;
}

/** Write a redirection entry's half; any other write is counted. */
static void chip_write(uint8_t id, uint32_t reg, uint32_t value)
{
    if (id != CHIP_ID || reg < 0x10 || reg >= 0x10 + 2 * CHIP_INPUTS) {
        bad_access++;
        return;
    }
    rte[(reg - 0x10) / 2][(reg - 0x10) % 2] = value;
}

static const struct ioapic_io chip_io = {chip_read, chip_write};

/** The chip's registers are never touched through the pair. */
uintptr_t map_mmio(uintptr_t phys_addr, size_t size)
{
    static uint32_t pair[8];

    (void)phys_addr;
    (void)size;
    return (uintptr_t)pair;
}

static struct acpi_info info;

/** The tables `chip_begin` describes. */
const struct acpi_info *acpi_get(void)
{
    return &info;
}

/** Line 0 comes to input 2, as the MADT's usual override has it; every
 * other line to its own input; a PCI device's line is level, active low. */
int acpi_isa_route(const struct acpi_info *a, int line, int pci, struct acpi_route *out)
{
    (void)a;
    out->gsi = line == 0 ? 2u : (uint32_t)line;
    out->level = pci;
    out->low = pci;
    return 0;
}

/* The vectors raised by a self-interrupt, in order. */
static uint8_t raised[32];
static int nraised;

/** Record a self-interrupt. */
void lapic_self_ipi(uint8_t vector)
{
    if (nraised < (int)sizeof(raised)) {
        raised[nraised] = vector;
    }
    nraised++;
}

/* The 8259 pair's mask registers, and the writes of them. */
static uint8_t imr_master, imr_slave;
static int pic_writes;

/** A write of a data port sets its mask register; the command ports are
 * passed over. */
void outb(uint16_t port, uint8_t val)
{
    if (port == 0x21) {
        imr_master = val;
        pic_writes++;
    } else if (port == 0xA1) {
        imr_slave = val;
        pic_writes++;
    }
}

/** A read of a data port gives its mask register. */
uint8_t inb(uint16_t port)
{
    return port == 0x21 ? imr_master : port == 0xA1 ? imr_slave : 0;
}

/** Only a retired 8259's mask of one line holds it; no row does that. */
void isr_line_hold(int line)
{
    (void)line;
}

/* ---- the rows ---- */

/** A fresh chip under `ioapic_init`, with no remapper. */
static void chip_begin(void)
{
    int i;

    for (i = 0; i < CHIP_INPUTS; i++) {
        rte[i][0] = 0x1234u;
        rte[i][1] = 0xAB000000u;
    }
    bad_access = 0;
    nraised = 0;
    info.nioapics = 1;
    info.ioapics[0].id = CHIP_ID;
    info.ioapics[0].address = 0xFEC00000u;
    info.ioapics[0].gsi_base = 0;
    ioapic_set_io(&chip_io);
    (void)kcmdline_capture("");
}

/** Whether every entry's two halves equal `want`'s. */
static int entries_are(uint32_t want[CHIP_INPUTS][2])
{
    int i;

    for (i = 0; i < CHIP_INPUTS; i++) {
        if (rte[i][0] != want[i][0] || rte[i][1] != want[i][1]) {
            return 0;
        }
    }
    return 1;
}

/** Copy every entry into `out`. */
static void entries_save(uint32_t out[CHIP_INPUTS][2])
{
    int i;

    for (i = 0; i < CHIP_INPUTS; i++) {
        out[i][0] = rte[i][0];
        out[i][1] = rte[i][1];
    }
}

/**
 * Route the keyboard (line 1) and COM1 (line 4), edge-triggered, a PCI
 * device's line 9, level-triggered, and line 8, which is then masked on its
 * own; mask the routed lines and unmask them again.
 */
static void rows_mask(void)
{
    uint32_t before[CHIP_INPUTS][2];
    uint16_t was;
    int i, others_kept = 1;

    chip_begin();
    check(ioapic_init(&info) == 1 && bad_access == 0, "init: the chip made ready");
    for (i = 0; i < CHIP_INPUTS; i++) {
        others_kept = others_kept && rte[i][0] == 0x10000u;
    }
    check(others_kept, "init: every input masked");
    check(ioapic_route_line(1, 0, 33, 0) == 0 && ioapic_route_line(4, 0, 36, 0) == 0 &&
              ioapic_route_line(9, 1, 41, 0) == 0 && ioapic_route_line(8, 0, 40, 0) == 0,
          "route: four lines");
    ioapic_mask_line(8);
    check(rte[1][0] == 33u && rte[4][0] == 36u && rte[9][0] == (41u | 0xA000u) &&
              rte[8][0] == (40u | 0x10000u),
          "route: lines 1, 4 and 9 unmasked, 9 level and active low, 8 masked");
    entries_save(before);

    was = ioapic_mask_routed();
    check(was == ((1u << 1) | (1u << 4) | (1u << 9)), "mask: the lines that were unmasked");
    check((rte[1][0] & 0x10000u) != 0 && (rte[4][0] & 0x10000u) != 0 && (rte[9][0] & 0x10000u) != 0,
          "mask: each masked");
    for (i = 0, others_kept = 1; i < CHIP_INPUTS; i++) {
        others_kept = others_kept && (rte[i][0] & ~0x10000u) == (before[i][0] & ~0x10000u) &&
                      rte[i][1] == before[i][1];
    }
    check(others_kept && nraised == 0, "mask: no other bit of any entry changed, nothing raised");
    check(ioapic_mask_routed() == 0, "mask: a second mask finds none unmasked");

    ioapic_unmask_lines(was);
    check(entries_are(before), "unmask: every entry back as it was, line 8 still masked");
    check(nraised == 2 && raised[0] == 33 && raised[1] == 36,
          "unmask: the keyboard's and COM1's edge vectors raised once, the level line's not");

    nraised = 0;
    ioapic_unmask_lines((uint16_t)(1u << 5));
    check(entries_are(before) && nraised == 0, "unmask: a line not routed is passed over");
    check(bad_access == 0, "no register outside the table written");
}

static int map_calls;

/** A remapper that gives each entry the next index, its high half in the
 * remapped format. */
static int remap_map(uint8_t id, uint32_t low, uint32_t high, uint32_t *rhigh)
{
    (void)id;
    (void)low;
    (void)high;
    *rhigh = ((uint32_t)map_calls << 17) | (1u << 16);
    return map_calls++;
}

/** Give an entry back. */
static void remap_unmap(int handle)
{
    (void)handle;
}

static const struct ioapic_remap remap_ops = {remap_map, remap_unmap};

/** The same, the entries remapped: the high halves keep the remapped
 * format across the mask. */
static void rows_mask_remapped(void)
{
    uint32_t before[CHIP_INPUTS][2];
    uint16_t was;

    chip_begin();
    (void)ioapic_init(&info);
    (void)ioapic_route_line(1, 0, 33, 0);
    (void)ioapic_route_line(9, 1, 41, 0);
    map_calls = 0;
    check(ioapic_remap_start(&remap_ops) == 0 && map_calls == 2, "remap: two entries");
    check(rte[1][1] == (1u << 16) && rte[9][1] == ((1u << 17) | (1u << 16)),
          "remap: the high halves name the entries");
    entries_save(before);
    was = ioapic_mask_routed();
    ioapic_unmask_lines(was);
    check(was == ((1u << 1) | (1u << 9)) && entries_are(before),
          "remap: the entries back as they were, remapped");
    check(nraised == 1 && raised[0] == 33, "remap: the edge vector raised from the low half");
    ioapic_remap_stop();
    check(rte[1][1] == 0 && rte[9][1] == 0, "remap: stopped, the destinations back");
}

/** The 8259 pair's masks held and given back, and kept after it retires. */
static void rows_pic(void)
{
    uint16_t held;

    imr_master = 0xB8;
    imr_slave = 0x8E;
    held = pic_mask_all();
    check(held == 0x8EB8u && imr_master == 0xFF && imr_slave == 0xFF,
          "pic: the masks returned, master low, and every line masked");
    pic_restore_mask(held);
    check(imr_master == 0xB8 && imr_slave == 0x8E, "pic: the masks back as they were");

    pic_retire();
    held = pic_mask_all();
    pic_writes = 0;
    pic_restore_mask(held);
    check(held == 0xFFFFu && pic_writes == 0 && imr_master == 0xFF && imr_slave == 0xFF,
          "pic: retired, every line stays masked");
}

/* The boot lines and whether each keeps the I/O APIC off, as Linux's
 * maxcpus() and get_option read them. */
static const struct {
    const char *line;
    int off;
} parses[] = {
    {"", 0},
    {"noapic", 1},
    {"noapic=1", 1}, /* do_early_param runs parse_noapic for any value */
    {"noapic=0", 1}, /* which it does not read */
    {"noapicx", 0},  /* another name */
    {"maxcpus=0", 1},
    {"maxcpus=00", 1},
    {"maxcpus=0x0", 1},
    {"maxcpus=0X00", 1},
    {"maxcpus=-0", 1},
    {"maxcpus=0x", 1},         /* no hexadecimal digit: octal, the 0 */
    {"maxcpus=08", 1},         /* octal, which stops at the 8 */
    {"maxcpus=x", 1},          /* no digit: get_option stores 0 */
    {"maxcpus=4294967296", 1}, /* 2^32, which the int cuts to 0 */
    {"maxcpus=1", 0},
    {"maxcpus=10", 0},
    {"maxcpus=010", 0},
    {"maxcpus=0x10", 0},
    {"maxcpus=-1", 0},
    {"maxcpus=", 0}, /* empty: get_option stores nothing */
    {"maxcpus", 0},
    {"nomaxcpus=0", 0},
    {"maxcpus=4 maxcpus=0", 1}, /* each occurrence runs maxcpus() */
    {"maxcpus=0 maxcpus=4", 1}, /* and none undoes the disable */
    {"maxcpus=4 maxcpus= maxcpus=2", 0},
    {"maxcpus=000000000000000000000000000000000", 1}, /* 33 bytes, read in place */
    {"maxcpus=\"0\"", 1},                             /* next_arg drops a value's quotes */
    {"maxcpus=\"4\"", 0},
    {"\"maxcpus=0\"", 1},     /* and a word's */
    {"maxcpus=\"0", 1},       /* an opening quote alone */
    {"maxcpus=\"\"", 0},      /* empty once the quotes go */
    {"a=\"x maxcpus=0\"", 0}, /* a quoted space ends no word */
    {"-- maxcpus=0", 0},      /* parse_args stops at a bare double dash */
    {"\"--\" noapic", 0},     /* quoted too */
    {"maxcpus=0 --", 1},
    {"--=1 maxcpus=0", 1}, /* a double dash with a value is a parameter */
    {"--x noapic", 1},
};

/* The boot lines and what each lookup answers for a word: `kcmdline_has`,
 * a flag's whole word with no `=` or a name's exact value, `kcmdline_early`,
 * Linux's do_early_param, which runs for a name with any value, and
 * `kcmdline_setup`, Linux's obsolete_checksetup, which matches a word by
 * its first bytes; names with `-` and `_` alike, as parameq has them. */
static const struct {
    const char *line;
    const char *word;
    int has, early, setup;
} words[] = {
    {"noapic", "noapic", 1, 1, 1},
    {"noapic=1", "noapic", 0, 1, 1},
    {"no-apic", "noapic", 0, 0, 0}, /* only a `-` that is a `_` is alike */
    {"nox2apic=0", "nox2apic", 0, 1, 1},
    {"intel-iommu=off", "intel_iommu=off", 1, 0, 1},
    {"intel_iommu=off", "intel-iommu=off", 1, 0, 1},
    {"intel_iommu=offx", "intel_iommu=off", 0, 0, 1},
    {"intel_iommu=of", "intel_iommu=off", 0, 0, 0},
    {"nomodeset", "nomodeset", 1, 1, 1},
    {"nomodeset=1", "nomodeset", 0, 1, 1},
    {"nomodesetx", "nomodeset", 0, 0, 1},
    {"nomode", "nomodeset", 0, 0, 0},
    {"\"nomodeset\"", "nomodeset", 1, 1, 1},
    {"nowatchdogs", "nowatchdog", 0, 0, 1},
    {"no_watchdog", "no-watchdog", 1, 1, 1},
    {"nosoftlockup=0", "nosoftlockup", 0, 1, 1},
    {"oops=panic", "oops=panic", 1, 0, 1},
    {"oops=\"panic\"", "oops=panic", 1, 0, 1}, /* next_arg drops the quotes */
    {"oops=panicx", "oops=panic", 0, 0, 1},
    {"oops=pani", "oops=panic", 0, 0, 0},
    {"oops", "oops=panic", 0, 0, 0},
    {"oops=panic", "oops", 0, 1, 1},
    {"xoops=panic", "oops=panic", 0, 0, 0},
    {"init=rescue", "init=rescue", 1, 0, 1},
    {"rach2.root=disk", "rach2.root=disk", 1, 0, 1},
    {"rach2.root=disk2", "rach2.root=disk", 0, 0, 1},
    {"rach2.root=ext2 rach2.root=disk", "rach2.root=disk", 1, 0, 1},
    {"-- nomodeset", "nomodeset", 0, 0, 0}, /* init's words */
};

/** Each line of `words` with `kcmdline_has`, `kcmdline_early` and
 * `kcmdline_setup`. */
static void rows_words(void)
{
    unsigned int i;

    for (i = 0; i < sizeof(words) / sizeof(words[0]); i++) {
        (void)kcmdline_capture(words[i].line);
        if ((kcmdline_has(words[i].word) != 0) != words[i].has ||
            (kcmdline_early(words[i].word) != 0) != words[i].early ||
            (kcmdline_setup(words[i].word) != 0) != words[i].setup) {
            failures++;
            printf("ioapic_hosttest: fail: words: \"%s\" for \"%s\": has %d, early %d, "
                   "setup %d\n",
                   words[i].line, words[i].word, kcmdline_has(words[i].word),
                   kcmdline_early(words[i].word), kcmdline_setup(words[i].word));
        }
    }
}

/** The `maxcpus=` parse of every occurrence, and `kcmdline_option`'s values. */
static void rows_parse(void)
{
    unsigned int i;
    int v = 7;

    for (i = 0; i < sizeof(parses) / sizeof(parses[0]); i++) {
        (void)kcmdline_capture(parses[i].line);
        if (ioapic_disabled() != parses[i].off) {
            failures++;
            printf("ioapic_hosttest: fail: parse: \"%s\" should %s the I/O APIC off\n",
                   parses[i].line, parses[i].off ? "keep" : "not keep");
        }
    }
    (void)kcmdline_capture("maxcpus=0x1f");
    check(kcmdline_option("maxcpus", &v) == 1 && v == 31, "option: hexadecimal");
    (void)kcmdline_capture("maxcpus=\"0x1f\"");
    check(kcmdline_option("maxcpus", &v) == 1 && v == 31, "option: quoted hexadecimal");
    (void)kcmdline_capture("maxcpus=017");
    check(kcmdline_option("maxcpus", &v) == 1 && v == 15, "option: octal");
    (void)kcmdline_capture("maxcpus=-2,3");
    check(kcmdline_option("maxcpus", &v) == 1 && v == -2, "option: negative, to the comma");
    v = 7;
    (void)kcmdline_capture("maxcpus=");
    check(kcmdline_option("maxcpus", &v) == 0 && v == 7, "option: empty, nothing stored");
    (void)kcmdline_capture("maxcpus=4 maxcpus=0 maxcpus=2");
    check(kcmdline_option("maxcpus", &v) == 1 && v == 2, "option: the last occurrence wins");
    (void)kcmdline_capture("maxcpus=4 maxcpus=");
    check(kcmdline_option("maxcpus", &v) == 1 && v == 4,
          "option: an empty last occurrence stores nothing");
    check(kcmdline_option_nth("maxcpus", 1, &v) == 0 && kcmdline_option_nth("maxcpus", 2, &v) == -1,
          "option: nth, empty and absent");
}

/** Mask and unmask before `ioapic_init`, as the late x2APIC leave does on
 * a machine whose I/O APIC never came up: no line is routed, so no
 * register is read or written and nothing is raised. */
static void rows_before_init(void)
{
    uint32_t before[CHIP_INPUTS][2];

    chip_begin();
    entries_save(before);
    check(ioapic_mask_routed() == 0, "before init: no line routed to mask");
    ioapic_unmask_lines(0xFFFFu);
    check(entries_are(before) && bad_access == 0 && nraised == 0,
          "before init: no register touched and nothing raised");
}

/* Boot lines and what Linux's setup_nointremap and setup_irqremap leave of
 * disable_irq_remap and no_x2apic_optout. */
static const struct {
    const char *line;
    int off;
    int optout;
} intremaps[] = {
    {"", 0, 0},
    {"intremap=off", 1, 0},
    {"nointremap", 1, 0},
    {"nointremap=1", 1, 0}, /* parse_args hands it on; the value is not read */
    {"nointremapx", 0, 0},
    {"xintremap=off", 0, 0},
    {"intremap", 0, 0}, /* no value: setup_irqremap refuses it */
    {"intremap=", 0, 0},
    {"intremap=on", 0, 0},
    {"intremap=on,off", 1, 0},
    {"intremap=off,on", 0, 0},             /* the later item wins */
    {"intremap=nosid intremap=off", 1, 0}, /* every token is read */
    {"intremap=off intremap=nosid", 1, 0}, /* and one without on or off keeps it */
    {"intremap=off intremap=on", 0, 0},
    {"nointremap intremap=on", 0, 0},
    {"intremap=on nointremap", 1, 0},
    {"intremap=offx", 1, 0}, /* strncmp over the word's length */
    {"intremap=onoff", 0, 0},
    {"intremap=of", 0, 0},
    {"intremap=,,off,,", 1, 0}, /* a run of commas is one */
    {"intremap=no_x2apic_optout", 0, 1},
    {"intremap=no_x2apic_optout,off", 1, 1},
    {"intremap=no_x2apic_optout intremap=on", 0, 1}, /* nothing clears it */
    {"intremap=nosid,no_x2apic_optoutx", 0, 1},
    {"intremap=no_x2apic", 0, 0},
    {"intremap=nosid,nosid,nosid,nosid,nosid,nosid,nosid,nosid,nosid,nosid,off", 1, 0},
};

/** The boot line's control of remapping, item by item and token by token,
 * and `kcmdline_list`'s answers under it. */
static void rows_intremap(void)
{
    static const char *const onoff[] = {"off", "on"};
    unsigned int i;
    int last;

    for (i = 0; i < sizeof(intremaps) / sizeof(intremaps[0]); i++) {
        (void)kcmdline_capture(intremaps[i].line);
        if (ioapic_intremap_off() != intremaps[i].off ||
            ioapic_no_x2apic_optout() != intremaps[i].optout) {
            failures++;
            printf("ioapic_hosttest: fail: intremap: \"%s\" should leave remapping %s and "
                   "the opt-out %s\n",
                   intremaps[i].line, intremaps[i].off ? "off" : "on",
                   intremaps[i].optout ? "overridden" : "kept");
        }
    }
    last = 7;
    (void)kcmdline_capture("intel_iommu=igfx_off,off intel_iommu=sm_on");
    check(kcmdline_list("intel_iommu", NULL, onoff, 2, &last) == 1 && last == 0,
          "list: the items that match no word are passed over");
    (void)kcmdline_capture("intel_iommu=off intel_iommu=igfx_off intel_iommu=on");
    check(kcmdline_list("intel_iommu", NULL, onoff, 2, &last) == 3 && last == 1,
          "list: the mask of every word, and the last");
    last = 7;
    (void)kcmdline_capture("intel_iommu=igfx_off nointel_iommu=off");
    check(kcmdline_list("intel_iommu", NULL, onoff, 2, &last) == 0 && last == 7,
          "list: none matched, the last left alone");
}

/* The DMAR table, its flags and boot lines, and whether remapping may
 * start in the x2APIC format on the x86-64 kernel, as Linux's early
 * intel_prepare_irq_remapping decides it. */
static const struct {
    int dmar;
    uint8_t flags;
    const char *line;
    int remappable;
} remappables[] = {
    {0, 0, "", 0},
    {1, 0, "", 0},
    {1, ACPI_DMAR_INTR_REMAP, "", 1},
    {1, ACPI_DMAR_INTR_REMAP, "intremap=off", 0},
    {1, ACPI_DMAR_INTR_REMAP, "nointremap", 0},
    {1, ACPI_DMAR_INTR_REMAP, "intremap=off,on", 1},
    {1, ACPI_DMAR_INTR_REMAP, "noapic", 0},
    {1, ACPI_DMAR_INTR_REMAP, "maxcpus=0", 0},
    {1, ACPI_DMAR_INTR_REMAP | ACPI_DMAR_X2APIC_OPT_OUT, "", 0},
    {1, ACPI_DMAR_INTR_REMAP | ACPI_DMAR_X2APIC_OPT_OUT, "intremap=no_x2apic_optout", 1},
    {1, ACPI_DMAR_X2APIC_OPT_OUT, "intremap=no_x2apic_optout", 0},
    {0, ACPI_DMAR_INTR_REMAP, "", 0},
};

/** `ioapic_remappable` from the DMAR table's flags and the boot line; only
 * the x86-64 kernel builds VT-d, so it is 0 on every other. */
static void rows_remappable(void)
{
    unsigned int i;
    int want;

    for (i = 0; i < sizeof(remappables) / sizeof(remappables[0]); i++) {
        info.dmar = remappables[i].dmar;
        info.dmar_flags = remappables[i].flags;
        (void)kcmdline_capture(remappables[i].line);
#if defined(__x86_64__)
        want = remappables[i].remappable;
#else
        want = 0;
#endif
        if (ioapic_remappable() != want) {
            failures++;
            printf(
                "ioapic_hosttest: fail: remappable: dmar %d, flags 0x%x, \"%s\" should give %d\n",
                remappables[i].dmar, (unsigned int)remappables[i].flags, remappables[i].line, want);
        }
    }
    info.dmar = 0;
    info.dmar_flags = 0;
}

int main(void)
{
    rows_before_init();
    rows_mask();
    rows_mask_remapped();
    rows_pic();
    rows_parse();
    rows_words();
    rows_intremap();
    rows_remappable();
    if (failures != 0) {
        printf("ioapic_hosttest: %d failed\n", failures);
        return 1;
    }
    printf("ioapic_hosttest: ok\n");
    return 0;
}
