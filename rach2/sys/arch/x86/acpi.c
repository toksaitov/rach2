/*
 * acpi.c: the firmware's ACPI tables, as acpi.h describes.
 *
 * The parser reads memory only through the map function it is given, so a
 * host test can run it over tables captured from QEMU and over tables made
 * wrong on purpose. Every multi-byte field is assembled from bytes, little
 * endian, since a table's fields are not aligned (an XSDT's 64-bit entries
 * start at offset 36).
 */

#include "acpi.h"
#include "portio.h"

#include "../../klog.h"
#include "../../mm/paging.h"
#include "../../pci.h"

#include "../../lib/kstring.h"

/* A table longer than this is refused: no firmware's DSDT comes near it,
 * and a length field that says more is a corrupt one. */
#define ACPI_TABLE_MAX 0x400000u

#define ACPI_HEADER_LEN 36

/** The little-endian 16-bit value at `p`. */
static uint32_t get16(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8);
}

/** The little-endian 32-bit value at `p`. */
static uint32_t get32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/** The little-endian 64-bit value at `p`. */
static uint64_t get64(const uint8_t *p)
{
    return (uint64_t)get32(p) | ((uint64_t)get32(p + 4) << 32);
}

/** The low half of the physical address `v`, for the log. */
static uint32_t lo32(uint64_t v)
{
    return (uint32_t)(v & 0xFFFFFFFFu);
}

/** The high half of the physical address `v`, for the log. */
static uint32_t hi32(uint64_t v)
{
    return (uint32_t)(v >> 32);
}

/** Whether the `n` bytes at `p` are the text `want`. */
static int same(const void *p, const char *want, uint32_t n)
{
    const uint8_t *b = (const uint8_t *)p;
    uint32_t i;

    for (i = 0; i < n; i++) {
        if (b[i] != (uint8_t)want[i]) {
            return 0;
        }
    }
    return 1;
}

int acpi_checksum_ok(const uint8_t *p, uint32_t len)
{
    uint8_t sum = 0;
    uint32_t i;

    for (i = 0; i < len; i++) {
        sum = (uint8_t)(sum + p[i]);
    }
    return sum == 0;
}

/** A table's signature as text, with a byte that is not a letter, a digit,
 * a space or an underscore shown as `?`, so that a corrupt header puts no
 * control byte in the log. */
static void sig_text(const uint8_t *p, char *out)
{
    int i;

    for (i = 0; i < 4; i++) {
        char c = (char)p[i];

        out[i] = ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                  c == ' ' || c == '_')
                     ? c
                     : '?';
    }
    out[4] = '\0';
}

/**
 * Whether the `avail` bytes at `p` start an RSDP: its signature, the
 * checksum of its first 20 bytes, and for revision 2 and later a length of
 * at least 36 bytes whose extended checksum is good too.
 */
static int rsdp_valid(const uint8_t *p, uint32_t avail)
{
    uint32_t len;

    if (avail < 20 || !same(p, "RSD PTR ", 8) || !acpi_checksum_ok(p, 20)) {
        return 0;
    }
    if (p[15] < 2) {
        return 1;
    }
    if (avail < 36) {
        return 0;
    }
    len = get32(p + 20);
    return len >= 36 && len <= avail && acpi_checksum_ok(p, len);
}

/** The physical address of the first RSDP that passes on a 16-byte boundary
 * of the `len` bytes at `base`, or 0. */
static uint64_t rsdp_scan(acpi_map_fn map, uint64_t base, uint32_t len)
{
    const uint8_t *p = map(base, len);
    uint32_t off;

    if (!p) {
        return 0;
    }
    for (off = 0; off + 20 <= len; off += 16) {
        if (rsdp_valid(p + off, len - off)) {
            return base + off;
        }
    }
    return 0;
}

uint64_t acpi_rsdp_search(acpi_map_fn map)
{
    const uint8_t *bda = map(0x40E, 2);
    uint64_t found;
    uint32_t ebda;

    if (bda) {
        /* The BIOS data area's word at 0x40E: the EBDA's segment. */
        ebda = get16(bda) << 4;
        if (ebda >= 0x400u && ebda + 1024u <= 0xA0000u) {
            found = rsdp_scan(map, ebda, 1024);
            if (found) {
                return found;
            }
        }
    }
    return rsdp_scan(map, 0xE0000u, 0x20000u);
}

/**
 * Map the table at `phys` whole and check it: a header, a length no
 * shorter than the header and than `min`, the signature `want` when one is
 * given, and its checksum. The table, with its length in `*len`, or NULL
 * after a line in the log that says why.
 */
static const uint8_t *table_read(acpi_map_fn map, uint64_t phys, const char *want, uint32_t min,
                                 uint32_t *len)
{
    const uint8_t *p = map(phys, ACPI_HEADER_LEN);
    char sig[5];
    uint32_t n;

    if (!p) {
        klog(KLOG_WARN, "acpi", "%s at 0x%x%08x: out of the kernel's reach, ignored",
             want ? want : "table", hi32(phys), lo32(phys));
        return 0;
    }
    sig_text(p, sig);
    if (want && !same(p, want, 4)) {
        klog(KLOG_WARN, "acpi", "%s at 0x%x: signature %s, ignored", want, lo32(phys), sig);
        return 0;
    }
    n = get32(p + 4);
    if (n < ACPI_HEADER_LEN || n < min || n > ACPI_TABLE_MAX) {
        klog(KLOG_WARN, "acpi", "%s at 0x%x: length %d, ignored", sig, lo32(phys), (int)n);
        return 0;
    }
    p = map(phys, n);
    if (!p) {
        klog(KLOG_WARN, "acpi", "%s at 0x%x: %d bytes out of the kernel's reach, ignored", sig,
             lo32(phys), (int)n);
        return 0;
    }
    if (!acpi_checksum_ok(p, n)) {
        klog(KLOG_WARN, "acpi", "%s at 0x%x: checksum fails, ignored", sig, lo32(phys));
        return 0;
    }
    *len = n;
    return p;
}

/** Read the 12-byte Generic Address Structure at `p` into `g`. */
static void gas_read(const uint8_t *p, struct acpi_gas *g)
{
    g->space = p[0];
    g->bit_width = p[1];
    g->bit_offset = p[2];
    g->access_size = p[3];
    g->address = get64(p + 4);
}

/** An I/O port register given by one of the FADT's 32-bit block fields. */
static void gas_port(struct acpi_gas *g, uint32_t port, uint32_t bytes)
{
    k_memset(g, 0, sizeof(*g));
    if (port != 0 && bytes != 0) {
        g->space = ACPI_SPACE_IO;
        g->bit_width = (uint8_t)(bytes * 8u);
        g->address = port;
    }
}

/** Take the FADT's 64-bit form of a register at `off` when it is there and
 * not empty, in place of the 32-bit block field already in `g`. */
static void gas_extended(const uint8_t *t, uint32_t len, uint32_t off, struct acpi_gas *g)
{
    struct acpi_gas ext;

    if (off + 12 > len) {
        return;
    }
    gas_read(t + off, &ext);
    if (ext.address != 0 &&
        (ext.space != ACPI_SPACE_MEMORY || ext.address <= (uint64_t)(uintptr_t)-1)) {
        if (ext.bit_width == 0) {
            ext.bit_width = g->bit_width;
        }
        *g = ext;
    }
}

/**
 * Add a processor to the record, once for each APIC identity, since one may
 * be listed by both an xAPIC and an x2APIC entry. With the table full the
 * entry counts as dropped, and an enabled processor takes the place of the
 * first one that is not, so that the processors that can run are kept.
 */
static void add_cpu(struct acpi_info *a, uint32_t apic_id, uint32_t uid, uint32_t flags)
{
    int i;

    for (i = 0; i < a->ncpus; i++) {
        if (a->cpus[i].apic_id == apic_id) {
            return; /* listed as both an xAPIC and an x2APIC entry */
        }
    }
    i = a->ncpus;
    if (i >= ACPI_MAX_CPUS) {
        a->madt_dropped++;
        if (!(flags & ACPI_CPU_ENABLED)) {
            return;
        }
        for (i = 0; i < a->ncpus && (a->cpus[i].flags & ACPI_CPU_ENABLED); i++) {
        }
        if (i == a->ncpus) {
            return;
        }
    } else {
        a->ncpus++;
    }
    a->cpus[i].apic_id = apic_id;
    a->cpus[i].uid = uid;
    a->cpus[i].flags = flags;
}

/** Add a local APIC's NMI input to the record; past ACPI_MAX_NMIS it counts
 * as dropped. */
static void add_nmi(struct acpi_info *a, uint32_t uid, uint16_t flags, uint8_t lint)
{
    if (a->nnmis >= ACPI_MAX_NMIS) {
        a->madt_dropped++;
        return;
    }
    a->nmis[a->nnmis].uid = uid;
    a->nmis[a->nnmis].flags = flags;
    a->nmis[a->nnmis].lint = lint;
    a->nnmis++;
}

/** The least length of an MADT entry of `type` that this kernel reads, and
 * 2 for a type it passes over. */
static uint32_t madt_entry_min(uint8_t type)
{
    switch (type) {
    case 0:
        return 8;
    case 1:
        return 12;
    case 2:
        return 10;
    case 3:
        return 8;
    case 4:
        return 6;
    case 5:
        return 12;
    case 9:
        return 16;
    case 10:
        return 12;
    default:
        return 2;
    }
}

/**
 * The MADT's entries: the processors by local APIC (type 0) and by x2APIC
 * (type 9), the I/O APICs (1), the overrides (2), the NMI sources (3), the
 * local APICs' NMI inputs (4 and 10), and a 64-bit local APIC address (5).
 * Other types are passed over by their length. An entry that runs past the
 * table ends the walk; one too short for its type is skipped.
 */
static void madt_parse(struct acpi_info *a, const uint8_t *t, uint32_t len)
{
    uint32_t off = 44;

    a->madt = 1;
    a->lapic_address = get32(t + 36);
    a->madt_flags = get32(t + 40);
    while (off + 2 <= len) {
        const uint8_t *e = t + off;
        uint8_t type = e[0];
        uint32_t elen = e[1];

        if (elen < 2 || off + elen > len) {
            klog(KLOG_WARN, "acpi",
                 "APIC: entry at offset %d runs past the table; the rest ignored", (int)off);
            return;
        }
        if (elen < madt_entry_min(type)) {
            klog(KLOG_WARN, "acpi", "APIC: entry of type %d at offset %d is %d bytes, ignored",
                 (int)type, (int)off, (int)elen);
            off += elen;
            continue;
        }
        switch (type) {
        case 0:
            add_cpu(a, e[3], e[2], get32(e + 4));
            break;
        case 1:
            if (a->nioapics >= ACPI_MAX_IOAPICS) {
                a->madt_dropped++;
                break;
            }
            a->ioapics[a->nioapics].id = e[2];
            a->ioapics[a->nioapics].address = get32(e + 4);
            a->ioapics[a->nioapics].gsi_base = get32(e + 8);
            a->nioapics++;
            break;
        case 2:
            if (a->noverrides >= ACPI_MAX_OVERRIDES) {
                a->madt_dropped++;
                break;
            }
            a->overrides[a->noverrides].bus = e[2];
            a->overrides[a->noverrides].source = e[3];
            a->overrides[a->noverrides].gsi = get32(e + 4);
            a->overrides[a->noverrides].flags = (uint16_t)get16(e + 8);
            a->noverrides++;
            break;
        case 3:
            if (a->nnmi_sources >= ACPI_MAX_NMIS) {
                a->madt_dropped++;
                break;
            }
            a->nmi_sources[a->nnmi_sources].flags = (uint16_t)get16(e + 2);
            a->nmi_sources[a->nnmi_sources].gsi = get32(e + 4);
            a->nnmi_sources++;
            break;
        case 4:
            add_nmi(a, e[2] == 0xFF ? ACPI_NMI_ALL : e[2], (uint16_t)get16(e + 3), e[5]);
            break;
        case 5:
            a->lapic_address = get64(e + 4);
            break;
        case 9:
            add_cpu(a, get32(e + 4), get32(e + 12), get32(e + 8));
            break;
        case 10:
            add_nmi(a, get32(e + 4), (uint16_t)get16(e + 2), e[8]);
            break;
        default:
            break;
        }
        off += elen;
    }
}

/*
 * The FADT's fields, by offset. A field past the table's length is not
 * read: an ACPI 1.0 FADT ends at 116 bytes, before the reset register and
 * the 64-bit forms of the registers.
 */
#define FADT_DSDT          40
#define FADT_SCI_INT       46
#define FADT_SMI_CMD       48
#define FADT_ACPI_ENABLE   52
#define FADT_PM1A_CNT      64
#define FADT_PM1B_CNT      68
#define FADT_PM_TMR        76
#define FADT_PM1_CNT_LEN   89
#define FADT_PM_TMR_LEN    91
#define FADT_CENTURY       108
#define FADT_BOOT_ARCH     109
#define FADT_FLAGS         112
#define FADT_RESET_REG     116
#define FADT_RESET_VALUE   128
#define FADT_X_DSDT        140
#define FADT_X_PM1A_CNT    172
#define FADT_X_PM1B_CNT    184
#define FADT_X_PM_TMR      208
#define FADT_SLEEP_CONTROL 244
#define FADT_MIN_LEN       116

/**
 * Read the FADT `t` of `len` bytes into the record: the fields of ACPI 1.0,
 * then, as the table's revision and length allow, the boot flags, the reset
 * register and the 64-bit forms of the registers, which take the place of
 * the 32-bit ones where they are given.
 */
static void fadt_parse(struct acpi_info *a, const uint8_t *t, uint32_t len)
{
    a->fadt = 1;
    a->fadt_revision = t[8];
    a->dsdt = get32(t + FADT_DSDT);
    a->dsdt32 = a->dsdt;
    a->sci_irq = (uint16_t)get16(t + FADT_SCI_INT);
    a->smi_cmd = get32(t + FADT_SMI_CMD);
    a->acpi_enable = t[FADT_ACPI_ENABLE];
    gas_port(&a->pm1a_cnt, get32(t + FADT_PM1A_CNT), t[FADT_PM1_CNT_LEN]);
    gas_port(&a->pm1b_cnt, get32(t + FADT_PM1B_CNT), t[FADT_PM1_CNT_LEN]);
    gas_port(&a->pm_timer, get32(t + FADT_PM_TMR), t[FADT_PM_TMR_LEN] >= 4 ? 4u : 0u);
    a->century = t[FADT_CENTURY];
    a->fadt_flags = get32(t + FADT_FLAGS);
    a->pm_timer_bits = (a->fadt_flags & ACPI_FADT_TMR_VAL_EXT) ? 32 : 24;
    /* IAPC_BOOT_ARCH is reserved before FADT revision 3, ACPI 2.0's. */
    if (a->fadt_revision >= 3) {
        a->boot_flags = (uint16_t)get16(t + FADT_BOOT_ARCH);
    }
    if (len >= FADT_RESET_VALUE + 1 && a->fadt_revision >= 2) {
        gas_read(t + FADT_RESET_REG, &a->reset_reg);
        a->reset_value = t[FADT_RESET_VALUE];
        a->reset_supported =
            (a->fadt_flags & ACPI_FADT_RESET_REG_SUP) != 0 && a->reset_reg.address != 0;
    }
    if (a->fadt_revision >= 3) {
        if (len >= FADT_X_DSDT + 8 && get64(t + FADT_X_DSDT) != 0) {
            a->dsdt = get64(t + FADT_X_DSDT);
        }
        gas_extended(t, len, FADT_X_PM1A_CNT, &a->pm1a_cnt);
        gas_extended(t, len, FADT_X_PM1B_CNT, &a->pm1b_cnt);
        gas_extended(t, len, FADT_X_PM_TMR, &a->pm_timer);
        if (len >= FADT_SLEEP_CONTROL + 12) {
            gas_read(t + FADT_SLEEP_CONTROL, &a->sleep_control);
        }
    }
}

#define HPET_MIN_LEN 56

/** Read the HPET table `t`, at `phys`, into the record: the event timer
 * block's ID, its registers' address and its number. A table whose
 * registers are not in memory is logged and left out. */
static void hpet_parse(struct acpi_info *a, const uint8_t *t, uint64_t phys)
{
    struct acpi_gas g;

    gas_read(t + 40, &g);
    if (g.space != ACPI_SPACE_MEMORY || g.address == 0) {
        klog(KLOG_WARN, "acpi", "HPET at 0x%x: its registers are not in memory, ignored",
             lo32(phys));
        return;
    }
    a->hpet = 1;
    a->hpet_id = get32(t + 36);
    a->hpet_address = g.address;
    a->hpet_number = t[52];
}

#define MCFG_MIN_LEN 44

/** Read the MCFG's windows into the record, at most ACPI_MAX_ECAM; one with
 * no base, or with its last bus below its first, is passed over. */
static void mcfg_parse(struct acpi_info *a, const uint8_t *t, uint32_t len)
{
    uint32_t off;

    for (off = 44; off + 16 <= len; off += 16) {
        struct acpi_ecam *e;

        if (a->necam >= ACPI_MAX_ECAM) {
            return;
        }
        e = &a->ecam[a->necam];
        e->base = get64(t + off);
        e->segment = (uint16_t)get16(t + off + 8);
        e->bus_first = t[off + 10];
        e->bus_last = t[off + 11];
        if (e->bus_last >= e->bus_first && e->base != 0) {
            a->necam++;
        }
    }
}

/** Note a table in the record: its signature as `sig_text` shows it, its
 * address and length, and whether it passed its checks. Past
 * ACPI_MAX_TABLES it is not noted. */
static void table_note(struct acpi_info *a, const uint8_t *sig, uint64_t phys, uint32_t len, int ok)
{
    struct acpi_table *t;

    if (a->ntables >= ACPI_MAX_TABLES) {
        return;
    }
    t = &a->tables[a->ntables++];
    sig_text(sig, t->sig);
    t->address = phys;
    t->length = len;
    t->ok = ok;
}

/** One table of the root's list: read, noted, and parsed by its kind; only
 * the first table of each kind is parsed. */
static void table_take(acpi_map_fn map, struct acpi_info *a, uint64_t phys)
{
    const uint8_t *head = map(phys, ACPI_HEADER_LEN);
    const uint8_t *t;
    uint32_t len = 0, min = ACPI_HEADER_LEN;
    char sig[5];

    if (!head) {
        klog(KLOG_WARN, "acpi", "table at 0x%x%08x: out of the kernel's reach, ignored", hi32(phys),
             lo32(phys));
        return;
    }
    sig_text(head, sig);
    if (same(sig, "APIC", 4) || same(sig, "MCFG", 4)) {
        min = 44;
    } else if (same(sig, "FACP", 4)) {
        min = FADT_MIN_LEN;
    } else if (same(sig, "HPET", 4)) {
        min = HPET_MIN_LEN;
    }
    t = table_read(map, phys, 0, min, &len);
    table_note(a, head, phys, t ? len : get32(head + 4), t != 0);
    if (!t) {
        return;
    }
    if (same(sig, "APIC", 4) && !a->madt) {
        madt_parse(a, t, len);
    } else if (same(sig, "FACP", 4) && !a->fadt) {
        fadt_parse(a, t, len);
    } else if (same(sig, "HPET", 4) && !a->hpet) {
        hpet_parse(a, t, phys);
    } else if (same(sig, "MCFG", 4) && a->necam == 0) {
        mcfg_parse(a, t, len);
    }
}

/** Walk the RSDT (`wide` 0, 32-bit entries) or the XSDT (64-bit ones) at
 * `phys`: 0, or -1 when the table itself failed its checks. */
static int root_walk(acpi_map_fn map, struct acpi_info *a, uint64_t phys, int wide)
{
    const char *want = wide ? "XSDT" : "RSDT";
    uint32_t len = 0, off, step = wide ? 8u : 4u;
    const uint8_t *t = table_read(map, phys, want, ACPI_HEADER_LEN, &len);

    if (!t) {
        return -1;
    }
    a->xsdt = wide;
    a->root = phys;
    for (off = ACPI_HEADER_LEN; off + step <= len; off += step) {
        uint64_t entry = wide ? get64(t + off) : get32(t + off);

        if (entry != 0) {
            table_take(map, a, entry);
        }
    }
    return 0;
}

int acpi_parse(acpi_map_fn map, uint64_t rsdp, struct acpi_info *a)
{
    const uint8_t *p;
    uint64_t xsdt = 0;
    uint32_t rsdt;
    int walked = -1;

    k_memset(a, 0, sizeof(*a));
    p = map(rsdp, 20);
    if (p && p[15] >= 2) {
        p = map(rsdp, 36);
    }
    if (!p || !rsdp_valid(p, p[15] >= 2 ? 36u : 20u)) {
        klog(KLOG_WARN, "acpi", "RSDP at 0x%x: a checksum or its length fails, ignored",
             lo32(rsdp));
        return -1;
    }
    if (p[15] >= 2) {
        xsdt = get64(p + 24);
    }
    a->found = 1;
    a->rsdp = rsdp;
    a->revision = p[15];
    k_memcpy(a->oem, p + 9, 6);
    a->oem[6] = '\0';
    rsdt = get32(p + 16);
    if (xsdt != 0) {
        walked = root_walk(map, a, xsdt, 1);
    }
    if (walked != 0 && rsdt != 0) {
        walked = root_walk(map, a, rsdt, 0);
    }
    if (walked != 0) {
        klog(KLOG_WARN, "acpi", "no root table passed its checks; no tables read");
    }
    if (a->fadt && a->dsdt != 0) {
        uint32_t len = 0;
        const uint8_t *d = table_read(map, a->dsdt, "DSDT", ACPI_HEADER_LEN, &len);

        if (!d && a->dsdt32 != 0 && a->dsdt32 != a->dsdt) {
            klog(KLOG_WARN, "acpi", "dsdt: X_DSDT 0x%x%08x unreadable; the 32-bit DSDT is read",
                 (unsigned)(a->dsdt >> 32), (unsigned)a->dsdt);
            a->dsdt = a->dsdt32;
            d = table_read(map, a->dsdt, "DSDT", ACPI_HEADER_LEN, &len);
        }
        table_note(a, (const uint8_t *)"DSDT", a->dsdt, len, d != 0);
        if (d && acpi_s5_find(d + ACPI_HEADER_LEN, len - ACPI_HEADER_LEN, &a->slp_typa,
                              &a->slp_typb) == 0) {
            a->s5 = 1;
        }
    }
    return 0;
}

/**
 * One integer of a package: Zero, One, or a constant after BytePrefix,
 * WordPrefix or DWordPrefix. The bytes it took, or 0 when the element at
 * `p` is something else or runs past `end`.
 */
static uint32_t aml_integer(const uint8_t *p, const uint8_t *end, uint32_t *value)
{
    if (p >= end) {
        return 0;
    }
    switch (*p) {
    case 0x00: /* ZeroOp */
        *value = 0;
        return 1;
    case 0x01: /* OneOp */
        *value = 1;
        return 1;
    case 0x0A: /* BytePrefix */
        if (end - p < 2) {
            return 0;
        }
        *value = p[1];
        return 2;
    case 0x0B: /* WordPrefix */
        if (end - p < 3) {
            return 0;
        }
        *value = get16(p + 1);
        return 3;
    case 0x0C: /* DWordPrefix */
        if (end - p < 5) {
            return 0;
        }
        *value = get32(p + 1);
        return 5;
    default:
        return 0;
    }
}

/** The package of a `_S5_` name at `p`: PackageOp, its length and element
 * count, and two integers. 0 with the types set, -1 otherwise. */
static int s5_package(const uint8_t *p, const uint8_t *end, uint8_t *typa, uint8_t *typb)
{
    uint32_t a, b, n;

    if (p >= end || *p != 0x12) { /* PackageOp */
        return -1;
    }
    p++;
    /* PkgLength: bits 6 and 7 of its lead byte count the bytes after it. */
    if (p >= end || (uint32_t)(end - p) < 1u + (uint32_t)(*p >> 6) + 1u) {
        return -1;
    }
    p += 1 + (*p >> 6);
    if (*p < 2) { /* NumElements */
        return -1;
    }
    p++;
    n = aml_integer(p, end, &a);
    if (n == 0 || a > 7) {
        return -1;
    }
    p += n;
    n = aml_integer(p, end, &b);
    if (n == 0 || b > 7) {
        return -1;
    }
    *typa = (uint8_t)a;
    *typb = (uint8_t)b;
    return 0;
}

int acpi_s5_find(const uint8_t *aml, uint32_t len, uint8_t *typa, uint8_t *typb)
{
    const uint8_t *end = aml + len;
    uint32_t i;

    for (i = 0; i + 4 <= len; i++) {
        uint32_t name = i;

        if (aml[i] != '_' || aml[i + 1] != 'S' || aml[i + 2] != '5' || aml[i + 3] != '_') {
            continue;
        }
        /* NameOp before the name, or before a root prefix before it. */
        if (name >= 1 && aml[name - 1] == '\\') {
            name--;
        }
        if (name < 1 || aml[name - 1] != 0x08) {
            continue;
        }
        if (s5_package(aml + i + 4, end, typa, typb) == 0) {
            return 0;
        }
    }
    return -1;
}

int acpi_isa_route(const struct acpi_info *a, int line, int pci, struct acpi_route *out)
{
    uint32_t flags = 0, polarity, trigger;
    int i, found = 0;

    out->gsi = (uint32_t)line;
    for (i = 0; i < a->noverrides; i++) {
        if (a->overrides[i].bus == 0 && a->overrides[i].source == line) {
            out->gsi = a->overrides[i].gsi;
            flags = a->overrides[i].flags;
            found = 1;
            break;
        }
    }
    if (!found) {
        for (i = 0; i < a->noverrides; i++) {
            if (a->overrides[i].bus == 0 && a->overrides[i].gsi == (uint32_t)line) {
                return -1;
            }
        }
    }
    polarity = flags & ACPI_POLARITY_MASK;
    trigger = flags & ACPI_TRIGGER_MASK;
    out->low = pci ? 1 : 0;
    if (polarity == ACPI_POLARITY_HIGH) {
        out->low = 0;
    } else if (polarity == ACPI_POLARITY_LOW) {
        out->low = 1;
    }
    out->level = pci ? 1 : 0;
    if (!pci && trigger == ACPI_TRIGGER_EDGE) {
        out->level = 0;
    } else if (!pci && trigger == ACPI_TRIGGER_LEVEL) {
        out->level = 1;
    }
    return 0;
}

/* ---- the kernel's side: the mappings, the record and the report ---- */

static struct acpi_info info;

/*
 * The mappings made so far. A table's header and the table are usually in
 * one page, and the tables of one firmware in a few, so a request inside a
 * window already mapped is answered from it: `map_mmio` maps for the life
 * of the kernel and has no unmap. A request outside them once all are made
 * is refused, since a mapping not recorded would be made again at the next
 * read of the same register, and each would keep its part of the MMIO
 * region for good.
 */
#define ACPI_WINDOWS 64

static struct {
    uint64_t phys;
    uint32_t len;
    uintptr_t va;
} windows[ACPI_WINDOWS];
static int nwindows, windows_full;

/**
 * The kernel's `acpi_map_fn`: the `len` bytes at `phys` through a window
 * already made when one covers them, otherwise through a new window of
 * whole pages from `map_mmio`. NULL for no bytes, a range that wraps, one
 * past 4 GiB on i686, one `map_mmio` cannot map, or a new window once all
 * are made.
 */
static const uint8_t *kernel_map(uint64_t phys, uint32_t len)
{
    uint64_t start, end;
    uintptr_t va;
    int i;

    if (len == 0 || phys + len < phys) {
        return 0;
    }
    for (i = 0; i < nwindows; i++) {
        if (phys >= windows[i].phys && phys + len <= windows[i].phys + windows[i].len) {
            return (const uint8_t *)(windows[i].va + (uintptr_t)(phys - windows[i].phys));
        }
    }
    start = phys & ~(uint64_t)0xFFFu;
    end = (phys + len + 0xFFFu) & ~(uint64_t)0xFFFu;
    if (sizeof(uintptr_t) < 8 && end - 1 > (uint64_t)0xFFFFFFFFu) {
        return 0;
    }
    if (nwindows == ACPI_WINDOWS) {
        if (!windows_full) {
            windows_full = 1;
            klog(KLOG_WARN, "acpi", "windows: all %d made; 0x%x%08x not mapped", ACPI_WINDOWS,
                 (unsigned)(phys >> 32), (unsigned)phys);
        }
        return 0;
    }
    va = map_mmio((uintptr_t)start, (size_t)(end - start));
    if (va == 0) {
        return 0;
    }
    windows[nwindows].phys = start;
    windows[nwindows].len = (uint32_t)(end - start);
    windows[nwindows].va = va;
    nwindows++;
    return (const uint8_t *)(va + (uintptr_t)(phys - start));
}

const struct acpi_info *acpi_get(void)
{
    return &info;
}

/** The report: the tables, then a line for each of the four this kernel
 * reads. */
static void acpi_report(const struct acpi_info *a)
{
    char list[ACPI_MAX_TABLES * 5 + 1];
    int i, n = 0, enabled = 0;

    for (i = 0; i < a->ntables; i++) {
        if (!a->tables[i].ok) {
            continue;
        }
        if (n > 0) {
            list[n++] = ' ';
        }
        k_memcpy(list + n, a->tables[i].sig, 4);
        n += 4;
    }
    list[n] = '\0';
    klog(KLOG_INFO, "acpi", "tables: %s at 0x%x, revision %d, %s: %s", a->xsdt ? "xsdt" : "rsdt",
         lo32(a->root), (int)a->revision, a->oem, n ? list : "none");
    if (a->madt) {
        for (i = 0; i < a->ncpus; i++) {
            if (a->cpus[i].flags & (ACPI_CPU_ENABLED | ACPI_CPU_ONLINE_CAPABLE)) {
                enabled++;
            }
        }
        klog(KLOG_INFO, "acpi",
             "madt: %d processors (%d usable), %d ioapics, %d overrides, %d nmi inputs%s", a->ncpus,
             enabled, a->nioapics, a->noverrides, a->nnmis,
             (a->madt_flags & 1u) ? ", an 8259 pair" : "");
        for (i = 0; i < a->noverrides; i++) {
            klog(KLOG_DEBUG, "acpi", "madt: override: isa %d to gsi %d, flags 0x%x",
                 (int)a->overrides[i].source, (int)a->overrides[i].gsi,
                 (unsigned)a->overrides[i].flags);
        }
        if (a->madt_dropped) {
            klog(KLOG_WARN, "acpi", "madt: %d entries past the kernel's limits, left out",
                 a->madt_dropped);
        }
    }
    if (a->fadt) {
        klog(KLOG_INFO, "acpi",
             "fadt: revision %d, pm1a 0x%x, pm timer 0x%x (%d-bit), reset %s, 8042 %s, "
             "century %d",
             (int)a->fadt_revision, lo32(a->pm1a_cnt.address), lo32(a->pm_timer.address),
             a->pm_timer_bits, a->reset_supported ? "register" : "none",
             (a->fadt_revision < 3 || (a->boot_flags & ACPI_BOOT_8042)) ? "yes" : "no",
             (int)a->century);
        if (a->s5) {
            klog(KLOG_INFO, "acpi", "dsdt: \\_S5 sleep types %d and %d", (int)a->slp_typa,
                 (int)a->slp_typb);
        } else {
            klog(KLOG_WARN, "acpi", "dsdt: no \\_S5 package found");
        }
    }
    if (a->hpet) {
        klog(KLOG_INFO, "acpi", "hpet: at 0x%x", lo32(a->hpet_address));
    }
    for (i = 0; i < a->necam; i++) {
        klog(KLOG_INFO, "acpi", "mcfg: segment %d, buses %d to %d at 0x%x", (int)a->ecam[i].segment,
             (int)a->ecam[i].bus_first, (int)a->ecam[i].bus_last, lo32(a->ecam[i].base));
    }
}

void acpi_init(uint64_t rsdp)
{
    const char *from = "the boot record";

    if (rsdp == 0) {
        rsdp = acpi_rsdp_search(kernel_map);
        from = "the bios area";
    }
    if (rsdp == 0) {
        k_memset(&info, 0, sizeof(info));
        klog(KLOG_INFO, "acpi", "none: no RSDP in the bios area");
        return;
    }
    klog(KLOG_DEBUG, "acpi", "rsdp: at 0x%x, from %s", lo32(rsdp), from);
    if (acpi_parse(kernel_map, rsdp, &info) == 0) {
        acpi_report(&info);
    }
}

/* ---- the registers a Generic Address Structure gives ---- */

/** A memory register's mapped address, made once; 0 when it cannot be
 * made. */
static uintptr_t gas_memory(const struct acpi_gas *g, uint32_t bytes)
{
    const uint8_t *p = kernel_map(g->address, bytes);

    return (uintptr_t)p;
}

/** The bytes of one access to the register `g`: by its access size when it
 * gives one of 1 to 3 (byte, word, doubleword), otherwise the least of 1, 2
 * and 4 that holds its bit width. */
static uint32_t gas_bytes(const struct acpi_gas *g)
{
    if (g->access_size >= 1 && g->access_size <= 3) {
        return 1u << (g->access_size - 1);
    }
    if (g->bit_width > 16) {
        return 4;
    }
    return g->bit_width > 8 ? 2u : 1u;
}

int acpi_gas_read(const struct acpi_gas *g, uint32_t *value)
{
    uint32_t bytes = gas_bytes(g);
    uintptr_t m;

    if (g->address == 0) {
        return -1;
    }
    switch (g->space) {
    case ACPI_SPACE_IO:
        if (g->address > 0xFFFFu) {
            return -1;
        }
        *value = bytes == 4   ? ind((uint16_t)g->address)
                 : bytes == 2 ? (uint32_t)inw((uint16_t)g->address)
                              : (uint32_t)inb((uint16_t)g->address);
        return 0;
    case ACPI_SPACE_MEMORY:
        m = gas_memory(g, bytes);
        if (!m) {
            return -1;
        }
        *value = bytes == 4   ? *(volatile uint32_t *)m
                 : bytes == 2 ? (uint32_t)*(volatile uint16_t *)m
                              : (uint32_t)*(volatile uint8_t *)m;
        return 0;
    case ACPI_SPACE_PCI:
        /* Bus 0: device in bits 32 to 47, function 16 to 31, offset 0 to 15. */
        *value =
            pci_read_config32(0, (uint8_t)((g->address >> 32) & 0x1Fu),
                              (uint8_t)((g->address >> 16) & 0x7u), (uint8_t)(g->address & 0xFCu));
        *value >>= 8u * (uint32_t)(g->address & 3u);
        return 0;
    default:
        return -1;
    }
}

int acpi_gas_write(const struct acpi_gas *g, uint32_t value)
{
    uint32_t bytes = gas_bytes(g);
    uintptr_t m;

    if (g->address == 0) {
        return -1;
    }
    switch (g->space) {
    case ACPI_SPACE_IO:
        if (g->address > 0xFFFFu) {
            return -1;
        }
        if (bytes == 4) {
            outd((uint16_t)g->address, value);
        } else if (bytes == 2) {
            outw((uint16_t)g->address, (uint16_t)value);
        } else {
            outb((uint16_t)g->address, (uint8_t)value);
        }
        return 0;
    case ACPI_SPACE_MEMORY:
        m = gas_memory(g, bytes);
        if (!m) {
            return -1;
        }
        if (bytes == 4) {
            *(volatile uint32_t *)m = value;
        } else if (bytes == 2) {
            *(volatile uint16_t *)m = (uint16_t)value;
        } else {
            *(volatile uint8_t *)m = (uint8_t)value;
        }
        return 0;
    case ACPI_SPACE_PCI: {
        uint8_t dev = (uint8_t)((g->address >> 32) & 0x1Fu);
        uint8_t fn = (uint8_t)((g->address >> 16) & 0x7u);
        uint8_t off = (uint8_t)(g->address & 0xFFu);

        if (bytes == 1) {
            pci_write_config8(0, dev, fn, off, (uint8_t)value);
        } else if (bytes == 2) {
            pci_write_config16(0, dev, fn, (uint8_t)(off & 0xFEu), (uint16_t)value);
        } else {
            pci_write_config32(0, dev, fn, (uint8_t)(off & 0xFCu), value);
        }
        return 0;
    }
    default:
        return -1;
    }
}
