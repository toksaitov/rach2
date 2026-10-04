/*
 * acpi_hosttest.c: the ACPI table parser and the \_S5 search, against tables
 * QEMU built and tables made here to be wrong.
 *
 * host-deps: rach2/sys/arch/x86/acpi.c
 * host-deps: rach2/sys/lib/kstring.c rach2/sys/lib/kctypes.c
 *
 * The parser reads physical memory through the map function it is given,
 * and the kernel's side of it through `map_mmio`. Both are answered here by
 * a model of physical memory: a few regions, each a buffer at a physical
 * address, and a request that is not wholly inside one is refused, as a
 * table past the kernel's reach is. Into the model go the tables QEMU's
 * SeaBIOS left for `-machine pc` and `-machine q35` (acpi_qemu_tables.h),
 * at the addresses they had, and tables built here byte by byte with their
 * checksums made right or wrong on purpose: a root table with a bad
 * checksum, an RSDP with only an XSDT, an MADT with an override of each
 * polarity and trigger, and entries cut short.
 *
 * The rows check the record the parser filled and what it logged, never
 * how it got there: a table that fails must be absent from the record and
 * named in the log, and every table that passes must be read in full.
 */

#include <stdarg.h>

extern int printf(const char *fmt, ...);
extern int vsnprintf(char *buf, unsigned long n, const char *fmt, va_list ap);
extern char *strstr(const char *hay, const char *needle);

#include "../../rach2/sys/arch/x86/acpi.h"
#include "../../rach2/sys/lib/kdef.h"
#include "acpi_qemu_tables.h"

static int fails;

#define CHECK(cond, what)                   \
    do {                                    \
        if (!(cond)) {                      \
            printf("  FAIL: %s\n", (what)); \
            fails++;                        \
        }                                   \
    } while (0)

/* ---- the log ---- */

static char log_text[16384];
static unsigned long log_len;

void klog(int level, const char *subsystem, const char *format, ...)
{
    va_list ap;
    int n;

    (void)level;
    if (log_len + 300 >= sizeof(log_text)) {
        return;
    }
    n = 0;
    while (subsystem[n] != '\0') {
        log_text[log_len++] = subsystem[n++];
    }
    log_text[log_len++] = ':';
    log_text[log_len++] = ' ';
    va_start(ap, format);
    n = vsnprintf(log_text + log_len, 256, format, ap);
    va_end(ap);
    log_len += n > 255 ? 255u : (unsigned long)n;
    log_text[log_len++] = '\n';
    log_text[log_len] = '\0';
}

static void log_clear(void)
{
    log_len = 0;
    log_text[0] = '\0';
}

static int logged(const char *what)
{
    return strstr(log_text, what) != 0;
}

/* ---- the model of physical memory ---- */

#define REGIONS    8
#define REGION_MAX 0x21000u

struct region {
    uint64_t phys;
    uint32_t len;
    unsigned char mem[REGION_MAX];
};

static struct region regions[REGIONS];
static int nregions;
static int mmio_calls;

static void model_reset(void)
{
    int i;
    uint32_t j;

    for (i = 0; i < REGIONS; i++) {
        for (j = 0; j < REGION_MAX; j++) {
            regions[i].mem[j] = 0;
        }
    }
    nregions = 0;
    mmio_calls = 0;
    log_clear();
}

/** A region of `len` bytes at `phys`, zeroed, rounded out to whole pages
 * as the kernel's mappings are. */
static unsigned char *region_add(uint64_t phys, uint32_t len)
{
    struct region *r = &regions[nregions++];

    r->phys = phys & ~(uint64_t)0xFFF;
    r->len = (uint32_t)(((phys + len + 0xFFF) & ~(uint64_t)0xFFF) - r->phys);
    if (r->len > REGION_MAX) {
        printf("  FAIL: a model region of %u bytes is too large\n", r->len);
        fails++;
        r->len = REGION_MAX;
    }
    return r->mem + (phys - r->phys);
}

static unsigned char *model_at(uint64_t phys, uint64_t len)
{
    int i;

    for (i = 0; i < nregions; i++) {
        if (phys >= regions[i].phys && phys + len <= regions[i].phys + regions[i].len) {
            return regions[i].mem + (phys - regions[i].phys);
        }
    }
    return 0;
}

static const uint8_t *model_map(uint64_t phys, uint32_t len)
{
    return model_at(phys, len);
}

uintptr_t map_mmio(uintptr_t phys, size_t size);

uintptr_t map_mmio(uintptr_t phys, size_t size)
{
    mmio_calls++;
    return (uintptr_t)model_at(phys, size);
}

/* ---- the ports and configuration space a register may be in ---- */

static uint32_t last_port, last_port_value, last_port_width;
static uint32_t last_cfg_dev, last_cfg_fn, last_cfg_off, last_cfg_value, last_cfg_width;

static void port_write(uint16_t port, uint32_t value, uint32_t width)
{
    last_port = port;
    last_port_value = value;
    last_port_width = width;
}

void outb(uint16_t port, uint8_t value)
{
    port_write(port, value, 8);
}

void outw(uint16_t port, uint16_t value)
{
    port_write(port, value, 16);
}

void outd(uint16_t port, uint32_t value)
{
    port_write(port, value, 32);
}

uint8_t inb(uint16_t port)
{
    return (uint8_t)(port & 0xFFu);
}

uint16_t inw(uint16_t port)
{
    return (uint16_t)(0x1200u | (port & 0xFFu));
}

uint32_t ind(uint16_t port)
{
    return 0x12345600u | (port & 0xFFu);
}

uint32_t pci_read_config32(uint8_t bus, uint8_t device, uint8_t function, uint8_t offset)
{
    (void)bus;
    return 0xA0000000u | ((uint32_t)device << 16) | ((uint32_t)function << 8) | offset;
}

static void cfg_write(uint8_t device, uint8_t function, uint8_t offset, uint32_t value,
                      uint32_t width)
{
    last_cfg_dev = device;
    last_cfg_fn = function;
    last_cfg_off = offset;
    last_cfg_value = value;
    last_cfg_width = width;
}

void pci_write_config32(uint8_t bus, uint8_t device, uint8_t function, uint8_t offset,
                        uint32_t value)
{
    (void)bus;
    cfg_write(device, function, offset, value, 32);
}

void pci_write_config16(uint8_t bus, uint8_t device, uint8_t function, uint8_t offset,
                        uint16_t value)
{
    (void)bus;
    cfg_write(device, function, offset, value, 16);
}

void pci_write_config8(uint8_t bus, uint8_t device, uint8_t function, uint8_t offset, uint8_t value)
{
    (void)bus;
    cfg_write(device, function, offset, value, 8);
}

/* ---- tables made here ---- */

static void put16(unsigned char *p, uint32_t v)
{
    p[0] = (unsigned char)v;
    p[1] = (unsigned char)(v >> 8);
}

static void put32(unsigned char *p, uint32_t v)
{
    put16(p, v & 0xFFFFu);
    put16(p + 2, v >> 16);
}

static void put64(unsigned char *p, uint64_t v)
{
    put32(p, (uint32_t)v);
    put32(p + 4, (uint32_t)(v >> 32));
}

static void checksum_fix(unsigned char *p, uint32_t len, uint32_t at)
{
    unsigned char sum = 0;
    uint32_t i;

    p[at] = 0;
    for (i = 0; i < len; i++) {
        sum = (unsigned char)(sum + p[i]);
    }
    p[at] = (unsigned char)(0x100u - sum);
}

/* Hand-made tables go one after another from here, in one region. */
#define MADE_AT  0x200000u
#define MADE_MAX 0x8000u

static unsigned char *made;
static uint32_t made_used;

static void made_reset(void)
{
    made = region_add(MADE_AT, MADE_MAX);
    made_used = 0;
}

/** A table of `sig` and revision `rev` around the `len` bytes of `body`,
 * its checksum right; its physical address. */
static uint64_t table_make(const char *sig, uint8_t rev, const unsigned char *body, uint32_t len)
{
    unsigned char *t = made + made_used;
    uint64_t at = MADE_AT + made_used;
    uint32_t i;

    for (i = 0; i < 4; i++) {
        t[i] = (unsigned char)sig[i];
    }
    put32(t + 4, 36 + len);
    t[8] = rev;
    for (i = 0; i < 6; i++) {
        t[10 + i] = (unsigned char)"RACH2 "[i];
    }
    for (i = 0; i < len; i++) {
        t[36 + i] = body[i];
    }
    checksum_fix(t, 36 + len, 9);
    made_used += (36 + len + 15u) & ~15u;
    return at;
}

/** A root table, the RSDT's 32-bit entries or the XSDT's 64-bit ones. */
static uint64_t root_make(int wide, const uint64_t *entries, int n)
{
    unsigned char body[64];
    int i;

    for (i = 0; i < n; i++) {
        if (wide) {
            put64(body + 8 * i, entries[i]);
        } else {
            put32(body + 4 * i, (uint32_t)entries[i]);
        }
    }
    return table_make(wide ? "XSDT" : "RSDT", 1, body, (uint32_t)(n * (wide ? 8 : 4)));
}

/** An RSDP at `at` in the BIOS area region `bios`, which starts at 0xE0000:
 * revision 0 with only an RSDT, or revision 2 with an XSDT beside it. */
static void rsdp_make(unsigned char *bios, uint32_t at, uint8_t rev, uint32_t rsdt, uint64_t xsdt)
{
    unsigned char *p = bios + (at - 0xE0000u);
    uint32_t i;

    for (i = 0; i < 8; i++) {
        p[i] = (unsigned char)"RSD PTR "[i];
    }
    for (i = 0; i < 6; i++) {
        p[9 + i] = (unsigned char)"RACH2 "[i];
    }
    p[15] = rev;
    put32(p + 16, rsdt);
    checksum_fix(p, 20, 8);
    if (rev >= 2) {
        put32(p + 20, 36);
        put64(p + 24, xsdt);
        checksum_fix(p, 36, 32);
    }
}

/* An MADT body: the local APIC address and flags, then the entries. */
static unsigned char madt_body[1024];
static uint32_t madt_len;

static void madt_start(void)
{
    put32(madt_body, 0xFEE00000u);
    put32(madt_body + 4, 1);
    madt_len = 8;
}

static void madt_entry(const unsigned char *e, uint32_t len)
{
    uint32_t i;

    for (i = 0; i < len; i++) {
        madt_body[madt_len + i] = e[i];
    }
    madt_len += len;
}

static void madt_override(uint8_t source, uint32_t gsi, uint16_t flags)
{
    unsigned char e[10] = {2, 10, 0, 0, 0, 0, 0, 0, 0, 0};

    e[3] = source;
    put32(e + 4, gsi);
    put16(e + 8, flags);
    madt_entry(e, sizeof(e));
}

/* ---- loading the captured machines ---- */

static unsigned char *bios_area(void)
{
    return region_add(0xE0000u, 0x20000u);
}

static void machine_load(const unsigned char *rsdp, uint32_t rsdp_at, const unsigned char *tables,
                         uint32_t tables_len, uint32_t tables_at)
{
    unsigned char *bios, *t;
    uint32_t i;

    model_reset();
    (void)region_add(0, 0x1000);
    bios = bios_area();
    for (i = 0; i < 20; i++) {
        bios[rsdp_at - 0xE0000u + i] = rsdp[i];
    }
    t = region_add(tables_at, tables_len);
    for (i = 0; i < tables_len; i++) {
        t[i] = tables[i];
    }
}

/* ---- the rows ---- */

static int has_table(const struct acpi_info *a, const char *sig, int ok)
{
    int i;

    for (i = 0; i < a->ntables; i++) {
        if (a->tables[i].sig[0] == sig[0] && a->tables[i].sig[1] == sig[1] &&
            a->tables[i].sig[2] == sig[2] && a->tables[i].sig[3] == sig[3] &&
            a->tables[i].ok == ok) {
            return 1;
        }
    }
    return 0;
}

/** QEMU's pc machine: an ACPI 1.0 RSDP in the BIOS area, an RSDT, a
 * revision 1 FADT with no reset register, no MCFG. */
static void qemu_pc_rows(void)
{
    struct acpi_info a;
    uint64_t at;

    printf("qemu pc\n");
    machine_load(qemu_pc_rsdp, QEMU_PC_RSDP_AT, qemu_pc_tables, sizeof(qemu_pc_tables),
                 QEMU_PC_TABLES_AT);
    at = acpi_rsdp_search(model_map);
    CHECK(at == QEMU_PC_RSDP_AT, "the RSDP is found in the BIOS area");
    CHECK(acpi_parse(model_map, at, &a) == 0, "the RSDP is taken");
    CHECK(a.found && a.revision == 0 && !a.xsdt, "an ACPI 1.0 RSDP leads to the RSDT");
    CHECK(has_table(&a, "FACP", 1) && has_table(&a, "APIC", 1) && has_table(&a, "HPET", 1) &&
              has_table(&a, "WAET", 1) && has_table(&a, "DSDT", 1),
          "every table passes its checks");
    CHECK(a.madt && a.lapic_address == 0xFEE00000u && (a.madt_flags & 1u), "the MADT's header");
    CHECK(a.ncpus == 1 && a.cpus[0].apic_id == 0 && (a.cpus[0].flags & ACPI_CPU_ENABLED),
          "one processor, enabled");
    CHECK(a.nioapics == 1 && a.ioapics[0].address == 0xFEC00000u && a.ioapics[0].gsi_base == 0,
          "one I/O APIC at 0xfec00000 from gsi 0");
    CHECK(a.noverrides == 5 && a.overrides[0].source == 0 && a.overrides[0].gsi == 2 &&
              a.overrides[0].flags == 0 && a.overrides[1].source == 5 &&
              a.overrides[1].flags == 0xD && a.overrides[4].source == 11,
          "the five overrides: line 0 to 2, and 5, 9, 10, 11 level and active high");
    CHECK(a.nnmis == 1 && a.nmis[0].uid == ACPI_NMI_ALL && a.nmis[0].lint == 1,
          "every processor's NMI on LINT1");
    CHECK(a.fadt && a.fadt_revision == 1, "a revision 1 FADT");
    CHECK(a.pm1a_cnt.space == ACPI_SPACE_IO && a.pm1a_cnt.address == 0x604 &&
              a.pm1a_cnt.bit_width == 16 && a.pm1b_cnt.address == 0,
          "PM1a control at port 0x604, sixteen bits, and no PM1b");
    CHECK(a.pm_timer.address == 0x608 && a.pm_timer_bits == 24, "a 24-bit PM timer at 0x608");
    CHECK(!a.reset_supported, "no reset register in an ACPI 1.0 FADT");
    CHECK(a.century == 0x32 && a.sci_irq == 9 && a.smi_cmd == 0xB2,
          "the century register, the SCI and the SMI command port");
    CHECK(a.boot_flags == 0, "no boot flags before revision 3");
    CHECK(a.s5 && a.slp_typa == 0 && a.slp_typb == 0, "\\_S5 is found with types 0 and 0");
    CHECK(a.hpet && a.hpet_address == 0xFED00000u, "the HPET at 0xfed00000");
    CHECK(a.necam == 0, "no MCFG on the pc machine");
    CHECK(!logged("ignored"), "nothing logged as ignored");
}

/** QEMU's q35 machine: a revision 3 FADT with the reset register and the
 * 8042 flag, and an MCFG. The kernel's side reads it, through `map_mmio`,
 * from the BIOS area search, and reports it. */
static void qemu_q35_rows(void)
{
    const struct acpi_info *a;
    struct acpi_route r;

    printf("qemu q35\n");
    machine_load(qemu_q35_rsdp, QEMU_Q35_RSDP_AT, qemu_q35_tables, sizeof(qemu_q35_tables),
                 QEMU_Q35_TABLES_AT);
    acpi_init(0);
    a = acpi_get();
    CHECK(a->found && a->rsdp == QEMU_Q35_RSDP_AT, "the kernel's side finds the RSDP");
    CHECK(has_table(a, "MCFG", 1) && has_table(a, "FACP", 1), "the MCFG and the FADT pass");
    CHECK(a->fadt_revision == 3 && a->reset_supported && a->reset_reg.space == ACPI_SPACE_IO &&
              a->reset_reg.address == 0xCF9 && a->reset_value == 0x0F,
          "the reset register: 0x0f to port 0xcf9");
    CHECK((a->boot_flags & ACPI_BOOT_8042) != 0, "the FADT says an 8042 is present");
    CHECK(a->necam == 1 && a->ecam[0].base == 0xB0000000u && a->ecam[0].segment == 0 &&
              a->ecam[0].bus_first == 0 && a->ecam[0].bus_last == 255,
          "the MCFG's window: segment 0, buses 0 to 255 at 0xb0000000");
    CHECK(a->s5 && a->slp_typa == 0 && a->slp_typb == 0, "\\_S5 in q35's DSDT");
    CHECK(logged("tables: rsdt at 0x7fe2345") && logged("MCFG"), "the report lists the tables");
    CHECK(mmio_calls > 0 && mmio_calls <= 8, "the tables share their mappings");
    CHECK(acpi_isa_route(a, 0, 0, &r) == 0 && r.gsi == 2 && !r.level && !r.low,
          "line 0 arrives at gsi 2, an edge, active high");
    CHECK(acpi_isa_route(a, 2, 0, &r) == -1, "line 2 has no input of its own");
    CHECK(acpi_isa_route(a, 1, 0, &r) == 0 && r.gsi == 1 && !r.level && !r.low,
          "line 1 conforms to ISA");
    CHECK(acpi_isa_route(a, 11, 1, &r) == 0 && r.gsi == 11 && r.level && !r.low,
          "a PCI device's line 11: level, and active high as its override says");
    CHECK(acpi_isa_route(a, 7, 1, &r) == 0 && r.gsi == 7 && r.level && r.low,
          "a PCI line no override gives: level and active low");

    acpi_init(QEMU_Q35_RSDP_AT);
    CHECK(acpi_get()->found && acpi_get()->madt, "an RSDP the boot record gives is taken");
}

/** An RSDP in the EBDA is found before one in the BIOS area, and a
 * signature with a bad checksum before it is passed over. */
static void search_rows(void)
{
    unsigned char *bda, *ebda, *bios;
    uint32_t i;

    printf("search\n");
    model_reset();
    bda = region_add(0, 0x1000);
    ebda = region_add(0x9F000u, 0x1000);
    bios = bios_area();
    made_reset();
    rsdp_make(bios, 0xF0000u, 0, 0x1000, 0);
    CHECK(acpi_rsdp_search(model_map) == 0xF0000u, "no EBDA: the BIOS area's");
    put16(bda + 0x40E, 0x9FC0u);
    for (i = 0; i < 8; i++) {
        ebda[0xC00 + 0x40 + i] = (unsigned char)"RSD PTR "[i];
    }
    CHECK(acpi_rsdp_search(model_map) == 0xF0000u, "a signature whose checksum fails is passed");
    for (i = 0; i < 20; i++) {
        ebda[0xC00 + 0x80 + i] = bios[0x10000 + i];
    }
    CHECK(acpi_rsdp_search(model_map) == 0x9FC80u, "the EBDA's first");
    for (i = 0; i < 20; i++) {
        ebda[0xC00 + 0x80 + i] = 0;
        bios[0x10000 + i] = 0;
    }
    CHECK(acpi_rsdp_search(model_map) == 0, "none at all");
}

/* Where the XSDT row puts its HPET table: past 4 GiB, with a low word at
 * which there is no table, so that an entry cut to 32 bits, or read as two
 * entries of 32, finds none. */
#define HIGH_AT 0x100300000ull

/** A revision 2 RSDP whose RSDT field is zero: the XSDT is read, its
 * entries 64 bits wide, one of them a table past 4 GiB. A bad XSDT falls
 * back to the RSDT; a bad extended checksum refuses the RSDP. */
static void xsdt_rows(void)
{
    struct acpi_info a;
    unsigned char *bios, *high, hpet[20] = {0};
    uint64_t t[2], xsdt, rsdt;
    uint32_t i;

    printf("xsdt\n");
    model_reset();
    bios = bios_area();
    made_reset();
    madt_start();
    t[0] = table_make("APIC", 3, madt_body, madt_len);
    hpet[4] = 0; /* memory space */
    put64(hpet + 8, 0xFED00000u);
    t[1] = table_make("HPET", 1, hpet, sizeof(hpet));
    high = region_add(HIGH_AT, 36 + sizeof(hpet));
    for (i = 0; i < 36 + sizeof(hpet); i++) {
        high[i] = made[t[1] - MADE_AT + i];
        made[t[1] - MADE_AT + i] = 0;
    }
    t[1] = HIGH_AT;
    xsdt = root_make(1, t, 2);
    rsdp_make(bios, 0xE0100u, 2, 0, xsdt);
    log_clear();
    CHECK(acpi_parse(model_map, 0xE0100u, &a) == 0 && a.xsdt && a.root == xsdt,
          "an XSDT-only RSDP leads to the XSDT");
    CHECK(a.madt && a.hpet && a.hpet_address == 0xFED00000u,
          "the XSDT's 64-bit entries are read, the HPET's past 4 GiB");
    CHECK(a.ntables == 2 && log_len == 0, "and nothing else is taken for an entry");

    rsdt = root_make(0, t, 1);
    made[xsdt - MADE_AT + 9] ^= 0x55;
    rsdp_make(bios, 0xE0200u, 2, (uint32_t)rsdt, xsdt);
    log_clear();
    CHECK(acpi_parse(model_map, 0xE0200u, &a) == 0 && !a.xsdt && a.root == rsdt && a.madt &&
              !a.hpet,
          "an XSDT that fails its checksum gives way to the RSDT");
    CHECK(logged("XSDT at 0x") && logged("checksum fails, ignored"), "the XSDT is logged");

    bios[0x200 + 32] ^= 1;
    log_clear();
    CHECK(acpi_parse(model_map, 0xE0200u, &a) == -1 && !a.found,
          "a bad extended checksum refuses the RSDP");
    CHECK(logged("RSDP at 0xe0200"), "the RSDP is logged");
}

/** A table with a bad checksum, one shorter than its header, one longer
 * than the memory there is, and one too short for its fields: each absent
 * from the record and named in the log, and the others still read. */
static void bad_table_rows(void)
{
    struct acpi_info a;
    unsigned char *bios, fadt[80] = {0}, hpet[20] = {0};
    uint64_t t[5];

    printf("bad tables\n");
    model_reset();
    bios = bios_area();
    made_reset();
    madt_start();
    madt_override(0, 2, 0);
    t[0] = table_make("APIC", 3, madt_body, madt_len);
    made[t[0] - MADE_AT + 40] ^= 0xFF;
    put64(hpet + 8, 0xFED00000u);
    t[1] = table_make("HPET", 1, hpet, sizeof(hpet));
    t[2] = table_make("SSDT", 1, fadt, 4);
    put32(made + (t[2] - MADE_AT) + 4, 20);
    t[3] = table_make("WAET", 1, fadt, 4);
    put32(made + (t[3] - MADE_AT) + 4, MADE_MAX * 2);
    t[4] = table_make("FACP", 1, fadt, 60);
    rsdp_make(bios, 0xE0000u, 0, (uint32_t)root_make(0, t, 5), 0);
    CHECK(acpi_parse(model_map, 0xE0000u, &a) == 0, "the RSDP is taken");
    CHECK(!a.madt && has_table(&a, "APIC", 0), "an MADT with a bad checksum is left out");
    CHECK(logged("APIC at 0x") && logged("checksum fails, ignored"), "and logged");
    CHECK(a.hpet, "the table after it is read");
    CHECK(has_table(&a, "SSDT", 0) && logged("SSDT at 0x") && logged("length 20, ignored"),
          "a length shorter than the header is refused");
    CHECK(has_table(&a, "WAET", 0) && logged("out of the kernel's reach"),
          "a length past the memory there is refused");
    CHECK(!a.fadt && has_table(&a, "FACP", 0) && logged("length 96, ignored"),
          "a FADT too short for its fields is refused");
    CHECK(!a.s5, "no \\_S5 without a FADT");
}

/** An MADT with an override of each polarity and trigger, an x2APIC
 * processor, an entry cut short and one that runs past the table. */
static void madt_rows(void)
{
    struct acpi_info a;
    struct acpi_route r;
    unsigned char *bios;
    unsigned char cpu[8] = {0, 8, 1, 3, 1, 0, 0, 0};
    unsigned char cpu_again[8] = {0, 8, 7, 3, 1, 0, 0, 0};
    unsigned char x2[16] = {9, 16, 0, 0, 0x20, 0x01, 0, 0, 1, 0, 0, 0, 0x55, 0, 0, 0};
    unsigned char ioapic[12] = {1, 12, 4, 0, 0, 0, 0xC0, 0xFE, 24, 0, 0, 0};
    unsigned char nmi[6] = {4, 6, 1, 0x0D, 0x00, 0};
    unsigned char lapic64[12] = {5, 12, 0, 0, 0, 0, 0xE0, 0xFE, 1, 0, 0, 0};
    unsigned char shortcpu[6] = {0, 6, 2, 9, 1, 0};
    uint64_t t[1];

    printf("madt\n");
    model_reset();
    bios = bios_area();
    made_reset();
    madt_start();
    madt_entry(cpu, sizeof(cpu));
    madt_entry(cpu_again, sizeof(cpu_again));
    madt_entry(x2, sizeof(x2));
    madt_entry(ioapic, sizeof(ioapic));
    madt_entry(nmi, sizeof(nmi));
    madt_entry(lapic64, sizeof(lapic64));
    madt_entry(shortcpu, sizeof(shortcpu));
    madt_override(1, 1, 0x0);   /* conforms */
    madt_override(3, 3, 0x1);   /* active high */
    madt_override(4, 4, 0x3);   /* active low */
    madt_override(5, 5, 0x4);   /* edge */
    madt_override(6, 6, 0xC);   /* level */
    madt_override(7, 7, 0xD);   /* level, active high */
    madt_override(8, 8, 0xF);   /* level, active low */
    madt_override(9, 9, 0x5);   /* edge, active high */
    madt_override(10, 10, 0x7); /* edge, active low */
    madt_override(0, 2, 0x0);
    madt_body[madt_len] = 2;
    madt_body[madt_len + 1] = 40; /* runs past the table */
    madt_len += 6;
    t[0] = table_make("APIC", 4, madt_body, madt_len);
    rsdp_make(bios, 0xE0000u, 0, (uint32_t)root_make(0, t, 1), 0);
    CHECK(acpi_parse(model_map, 0xE0000u, &a) == 0 && a.madt, "the MADT is read");
    CHECK(a.ncpus == 2 && a.cpus[0].apic_id == 3 && a.cpus[0].uid == 1 &&
              a.cpus[1].apic_id == 0x120 && a.cpus[1].uid == 0x55,
          "an xAPIC and an x2APIC processor, the repeated identity once");
    CHECK(a.nioapics == 1 && a.ioapics[0].id == 4 && a.ioapics[0].gsi_base == 24,
          "the I/O APIC's identity and base");
    CHECK(a.nnmis == 1 && a.nmis[0].uid == 1 && a.nmis[0].flags == 0x0D && a.nmis[0].lint == 0,
          "an NMI on processor 1's LINT0");
    CHECK(a.lapic_address == 0x1FEE00000ull, "the override's address replaces the header's");
    CHECK(logged("entry of type 0 at offset") && logged("is 6 bytes, ignored"),
          "an entry too short for its type is skipped");
    CHECK(logged("runs past the table"), "an entry past the end stops the walk");
    CHECK(a.noverrides == 10, "the ten overrides");

    CHECK(acpi_isa_route(&a, 1, 0, &r) == 0 && !r.level && !r.low, "conforms, ISA: edge, high");
    CHECK(acpi_isa_route(&a, 1, 1, &r) == 0 && r.level && r.low, "conforms, PCI: level, low");
    CHECK(acpi_isa_route(&a, 3, 0, &r) == 0 && !r.level && !r.low, "active high, ISA: edge");
    CHECK(acpi_isa_route(&a, 4, 0, &r) == 0 && !r.level && r.low, "active low, ISA: edge");
    CHECK(acpi_isa_route(&a, 4, 1, &r) == 0 && r.level && r.low, "active low, PCI: level");
    CHECK(acpi_isa_route(&a, 5, 0, &r) == 0 && !r.level && !r.low, "edge: high by ISA");
    CHECK(acpi_isa_route(&a, 6, 0, &r) == 0 && r.level && !r.low, "level: high by ISA");
    CHECK(acpi_isa_route(&a, 6, 1, &r) == 0 && r.level && r.low, "level: low by PCI");
    CHECK(acpi_isa_route(&a, 7, 0, &r) == 0 && r.level && !r.low, "level, active high");
    CHECK(acpi_isa_route(&a, 8, 0, &r) == 0 && r.level && r.low, "level, active low");
    CHECK(acpi_isa_route(&a, 9, 1, &r) == 0 && r.level && !r.low,
          "edge, active high, PCI: level whatever the override says");
    CHECK(acpi_isa_route(&a, 10, 0, &r) == 0 && !r.level && r.low, "edge, active low");
    CHECK(acpi_isa_route(&a, 0, 0, &r) == 0 && r.gsi == 2, "line 0 to gsi 2");
    CHECK(acpi_isa_route(&a, 2, 0, &r) == -1, "line 2's input is line 0's");
    CHECK(acpi_isa_route(&a, 12, 0, &r) == 0 && r.gsi == 12, "a line no override mentions");
}

/** A MADT with more processors than the kernel's 64 places: an enabled one
 * takes the place of a disabled one, and a disabled one past the end is
 * left out. */
static void madt_full_rows(void)
{
    struct acpi_info a;
    unsigned char *bios, e[8] = {0, 8, 0, 0, 0, 0, 0, 0};
    uint64_t t[1];
    int i, enabled = 0, seen = 0;

    printf("madt, full\n");
    model_reset();
    bios = bios_area();
    made_reset();
    madt_start();
    for (i = 0; i < 66; i++) {
        e[2] = (unsigned char)i;
        e[3] = (unsigned char)i;
        e[4] = (unsigned char)(i == 0 || i == 64 ? ACPI_CPU_ENABLED : 0);
        madt_entry(e, sizeof(e));
    }
    t[0] = table_make("APIC", 4, madt_body, madt_len);
    rsdp_make(bios, 0xE0000u, 0, (uint32_t)root_make(0, t, 1), 0);
    CHECK(acpi_parse(model_map, 0xE0000u, &a) == 0 && a.madt, "the MADT of 66 processors is read");
    for (i = 0; i < a.ncpus; i++) {
        if (a.cpus[i].flags & ACPI_CPU_ENABLED) {
            enabled++;
            seen |= a.cpus[i].apic_id == 64;
        }
    }
    CHECK(a.ncpus == 64 && enabled == 2 && seen && a.madt_dropped == 2,
          "processor 64, enabled, displaces a disabled one; the last disabled one is left out");
}

/** The FADT's 64-bit register forms win over its 32-bit block fields from
 * revision 3, and the DSDT's address with them. */
static void fadt_rows(void)
{
    struct acpi_info a;
    unsigned char *bios, body[244 - 36] = {0}, dsdt[16] = {0x08, '_',  'S',  '5',  '_',  0x12, 0x08,
                                                           0x04, 0x0A, 0x05, 0x0A, 0x06, 0,    0};
    uint64_t t[1], d;

    printf("fadt\n");
    model_reset();
    bios = bios_area();
    made_reset();
    d = table_make("DSDT", 2, dsdt, sizeof(dsdt));
    put32(body + 40 - 36, 0xDEAD000u); /* a 32-bit DSDT the 64-bit one replaces */
    put32(body + 64 - 36, 0x404);
    body[89 - 36] = 2;
    put32(body + 76 - 36, 0x408);
    body[91 - 36] = 4;
    put16(body + 109 - 36, ACPI_BOOT_8042);
    put32(body + 112 - 36, ACPI_FADT_TMR_VAL_EXT | ACPI_FADT_RESET_REG_SUP);
    body[116 - 36] = ACPI_SPACE_PCI;
    body[117 - 36] = 8;
    put64(body + 120 - 36, ((uint64_t)0x1F << 32) | (3u << 16) | 0x44u);
    body[128 - 36] = 0x06;
    put64(body + 140 - 36, d);
    body[172 - 36] = ACPI_SPACE_MEMORY;
    body[173 - 36] = 16;
    put64(body + 176 - 36, MADE_AT + 0x7000u);
    t[0] = table_make("FACP", 5, body, sizeof(body));
    rsdp_make(bios, 0xE0000u, 0, (uint32_t)root_make(0, t, 1), 0);
    CHECK(acpi_parse(model_map, 0xE0000u, &a) == 0 && a.fadt && a.fadt_revision == 5,
          "a revision 5 FADT");
    CHECK(a.dsdt == d && a.s5 && a.slp_typa == 5 && a.slp_typb == 6,
          "X_DSDT replaces DSDT, and its \\_S5 is read");
    CHECK(a.pm1a_cnt.space == ACPI_SPACE_MEMORY && a.pm1a_cnt.address == MADE_AT + 0x7000u,
          "X_PM1a_CNT_BLK replaces PM1a_CNT_BLK");
    CHECK(a.pm_timer.space == ACPI_SPACE_IO && a.pm_timer.address == 0x408 && a.pm_timer_bits == 32,
          "an empty X_PM_TMR_BLK leaves the 32-bit port, with its extended width");
    CHECK(a.reset_supported && a.reset_reg.space == ACPI_SPACE_PCI && a.reset_value == 6,
          "a reset register in configuration space");
    CHECK(a.boot_flags == ACPI_BOOT_8042, "the boot flags of a later FADT");

    CHECK(acpi_gas_write(&a.reset_reg, a.reset_value) == 0 && last_cfg_dev == 0x1F &&
              last_cfg_fn == 3 && last_cfg_off == 0x44 && last_cfg_value == 6 &&
              last_cfg_width == 8,
          "the reset value is one byte written to device 31, function 3, offset 0x44");
    CHECK(acpi_gas_write(&a.pm1a_cnt, 0x3C01) == 0 && made[0x7000] == 0x01 && made[0x7001] == 0x3C,
          "a PM1 control register in memory takes sixteen bits");
    {
        struct acpi_gas g = {ACPI_SPACE_IO, 16, 0, 0, 0xB2};
        uint32_t v = 0;

        CHECK(acpi_gas_write(&g, 0x1234) == 0 && last_port == 0xB2 && last_port_width == 16 &&
                  last_port_value == 0x1234,
              "a sixteen-bit port");
        CHECK(acpi_gas_read(&g, &v) == 0 && v == 0x12B2, "a sixteen-bit port read");
        g.address = 0x10000;
        CHECK(acpi_gas_write(&g, 1) == -1, "a port past 0xffff is refused");
    }

    model_reset();
    bios = bios_area();
    made_reset();
    d = table_make("DSDT", 2, dsdt, sizeof(dsdt));
    put32(body + 40 - 36, (uint32_t)d);
    put64(body + 140 - 36, 0x500000000ull); /* an X_DSDT nothing answers at */
    t[0] = table_make("FACP", 5, body, sizeof(body));
    rsdp_make(bios, 0xE0000u, 0, (uint32_t)root_make(0, t, 1), 0);
    CHECK(acpi_parse(model_map, 0xE0000u, &a) == 0 && a.dsdt == d && a.s5 && a.slp_typa == 5,
          "an X_DSDT that cannot be read leaves the DSDT, and its \\_S5 is read");
    CHECK(logged("X_DSDT 0x500000000 unreadable"), "the unreadable X_DSDT is named");
}

/** The kernel's windows on physical memory: once all are made, a register
 * outside them is refused and maps nothing, however often it is read, and
 * one inside them is still read. */
static void window_rows(void)
{
    struct acpi_gas g = {ACPI_SPACE_MEMORY, 32, 0, 3, 0};
    uint32_t v, page, calls;
    int refused = 0;

    printf("windows\n");
    model_reset();
    region_add(0x300000u, REGION_MAX);
    region_add(0x400000u, REGION_MAX);
    region_add(0x500000u, REGION_MAX);
    for (page = 0; page < 99 && !refused; page++) {
        g.address = 0x300000u + 0x100000u * (page / 33u) + 0x1000u * (page % 33u);
        refused = acpi_gas_read(&g, &v) != 0;
    }
    CHECK(refused, "the windows run out before 99 pages");
    calls = (uint32_t)mmio_calls;
    CHECK(acpi_gas_read(&g, &v) != 0 && acpi_gas_read(&g, &v) != 0 && (uint32_t)mmio_calls == calls,
          "a register past the windows is refused and maps nothing");
    CHECK(logged("windows: all 64 made"), "the full windows are named once");
    g.address = 0x300000u;
    CHECK(acpi_gas_read(&g, &v) == 0 && (uint32_t)mmio_calls == calls,
          "a register inside a window is read from it");
}

/* ---- \_S5 ---- */

static int s5(const unsigned char *aml, uint32_t len, int a, int b)
{
    uint8_t ta = 0xEE, tb = 0xEE;

    if (acpi_s5_find(aml, len, &ta, &tb) != 0) {
        return a < 0;
    }
    return ta == a && tb == b;
}

static void s5_rows(void)
{
    const unsigned char qemu[] = {0x08, '_', 'S', '5', '_', 0x12, 0x06, 0x04, 0, 0, 0, 0};
    const unsigned char bytes[] = {0x08, '_',  'S',  '5',  '_',  0x12, 0x0A,
                                   0x04, 0x0A, 0x07, 0x0A, 0x03, 0,    0};
    const unsigned char ones[] = {0x08, '_', 'S', '5', '_', 0x12, 0x06, 0x02, 0x01, 0x01};
    const unsigned char root[] = {0x08, '\\', '_', 'S', '5', '_', 0x12, 0x06, 0x04, 0x01, 0x00};
    const unsigned char words[] = {0x08, '_',  'S',  '5',  '_',  0x12, 0x0C, 0x02,
                                   0x0B, 0x05, 0x00, 0x0C, 0x02, 0,    0,    0};
    const unsigned char long_pkg[] = {0x08, '_',  'S',  '5',  '_',  0x12, 0x46,
                                      0x00, 0x04, 0x0A, 0x07, 0x00, 0,    0};
    const unsigned char no_name[] = {0x70, '_', 'S', '5', '_',  0x12, 0x06, 0x04, 0x01, 0x01, 0x08,
                                     '_',  'S', '5', '_', 0x12, 0x06, 0x04, 0x0A, 0x02, 0x0A, 0x03};
    const unsigned char one_elem[] = {0x08, '_', 'S', '5', '_', 0x12, 0x04, 0x01, 0x01};
    const unsigned char too_big[] = {0x08, '_', 'S', '5', '_', 0x12, 0x06, 0x02, 0x0A, 0x08, 0x00};
    const unsigned char not_pkg[] = {0x08, '_', 'S', '5', '_', 0x0A, 0x05};
    const unsigned char method[] = {0x08, '_', 'S', '5', '_', 0x12, 0x06, 0x02, 0x5F, 0x01};
    const unsigned char cut[] = {0x08, '_', 'S', '5', '_', 0x12, 0x06, 0x04, 0x0A};
    const unsigned char cut_len[] = {0x08, '_', 'S', '5', '_', 0x12, 0xC0};

    printf("\\_S5\n");
    CHECK(s5(qemu, sizeof(qemu), 0, 0), "QEMU's package of four zeros");
    CHECK(s5(bytes, sizeof(bytes), 7, 3), "BytePrefix values");
    CHECK(s5(ones, sizeof(ones), 1, 1), "OneOp values");
    CHECK(s5(root, sizeof(root), 1, 0), "the name after a root prefix");
    CHECK(s5(words, sizeof(words), 5, 2), "WordPrefix and DWordPrefix values");
    CHECK(s5(long_pkg, sizeof(long_pkg), 7, 0), "a two-byte PkgLength");
    CHECK(s5(no_name, sizeof(no_name), 2, 3), "a use of the name that is not its definition");
    CHECK(s5(one_elem, sizeof(one_elem), -1, -1), "one element is not enough");
    CHECK(s5(too_big, sizeof(too_big), -1, -1), "a sleep type past 7");
    CHECK(s5(not_pkg, sizeof(not_pkg), -1, -1), "a name that is not a package");
    CHECK(s5(method, sizeof(method), -1, -1), "an element that is not a constant");
    CHECK(s5(cut, sizeof(cut), -1, -1), "a package cut in its first element");
    CHECK(s5(cut_len, sizeof(cut_len), -1, -1), "a PkgLength cut short");
    CHECK(s5(qemu, 4, -1, -1), "the name alone");
    CHECK(s5(qemu_pc_tables + 0x40 + 36, 8605 - 36, 0, 0), "the captured pc DSDT");
}

int main(void)
{
    qemu_pc_rows();
    qemu_q35_rows();
    search_rows();
    xsdt_rows();
    bad_table_rows();
    madt_rows();
    madt_full_rows();
    fadt_rows();
    window_rows();
    s5_rows();
    if (fails) {
        printf("acpi_hosttest: %d check(s) failed\n", fails);
        return fails > 255 ? 255 : fails;
    }
    printf("acpi_hosttest: all checks passed\n");
    return 0;
}
