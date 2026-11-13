/*
 * vtd_hosttest.c: the VT-d driver of rach2/sys/arch/x86/iommu/ against a
 * model of the remapping units, over the DMAR tables of QEMU and the Lenovo.
 *
 * host-deps: rach2/sys/arch/x86/pciconf.c
 * host-deps: rach2/sys/lib/kstring.c rach2/sys/lib/kctypes.c
 * host-deps: tests/host/compat/kheap_host.c
 * host-deps: tests/host/compat/spinlock_host.c
 *
 * The model is written from the VT-d specification. Each unit has its
 * register file, the root and remapping table pointers it latched, and
 * three caches, of context entries, IOTLB entries and interrupt entries,
 * which only an invalidation empties and which in caching mode (CAP.CM)
 * also hold what was not present. A unit whose walks are not coherent
 * (ECAP.C clear, as on both of the Lenovo's units) reads its tables from a
 * copy of memory that only `vtd_hw_flush` updates, and a page starts there
 * as stale bytes, so an entry the driver did not write back reads as
 * garbage. The rows give the model DMA requests and interrupts and read
 * where they went, or the fault the unit recorded and fault.c reported.
 *
 * Configuration space is memory behind pciconf.c's ECAM accessors, and
 * sys/pci.c is included whole so that each row walks its own buses. The I/O
 * APIC is a list of redirection entries that this file's
 * `ioapic_remap_start` rewrites as ioapic.c does; QEMU boots check the real
 * one. The Lenovo's DMAR table is rebuilt from Linux's boot log
 * (dev-log/70-wifi-chip/linux/dmesg.txt:77 and 190-200): its length, 0xB0,
 * the two units with their capabilities, the reserved regions and the
 * I/O APIC and HPET ids are the log's; the units' scopes, the GPU at 00:02.0,
 * the I/O APIC at f0:1f.0 and the HPET at f0:0f.0, are as the trial of 81-6d
 * read them on the machine (dev-log/81-kvm/81-6d.md); the scopes of the
 * reserved regions (the USB controllers at 00:14.0 and 00:1d.0, and the GPU)
 * are the usual ones of Intel's chipsets, which the length confirms but
 * neither log gives.
 */

#include "../../rach2/sys/arch/x86/iommu/dmar.c"
#include "../../rach2/sys/arch/x86/iommu/vtd.c"
#include "../../rach2/sys/arch/x86/iommu/intremap.c"
#include "../../rach2/sys/arch/x86/iommu/fault.c"
#include "../../rach2/sys/pci.c"

#include "../../rach2/sys/arch/x86/pciconf.h"
#include "../../rach2/sys/lib/kirq.h"
#include "acpi_qemu_tables.h"

#include <stdarg.h>

extern int printf(const char *fmt, ...);
extern int snprintf(char *s, unsigned long n, const char *fmt, ...);
extern int vsnprintf(char *s, unsigned long n, const char *fmt, va_list ap);
extern char *strstr(const char *haystack, const char *needle);
extern void *aligned_alloc(unsigned long align, unsigned long size);
extern void *calloc(unsigned long n, unsigned long size);
extern void free(void *p);
extern void *memcpy(void *d, const void *s, unsigned long n);
extern void *memset(void *d, int c, unsigned long n);

#define DF(d, f) ((uint8_t)(((d) << 3) | (f)))

#define CAP_QEMU     0x80d2008c222f0606ull
#define ECAP_QEMU    0xf00f4aull
#define CAP_LENOVO0  0xc0000020660462ull
#define ECAP_LENOVO0 0xf0101aull
#define CAP_LENOVO1  0xd2008020660462ull
#define ECAP_LENOVO1 0xf010daull

#include "../../rach2/sys/proc/percpu.h"
#include "../../rach2/sys/arch/x86/smpboot.h"
#include "../../rach2/sys/arch/x86/lapic.h"

struct percpu cpu_table[NR_CPUS];
static uint32_t model_apic[NR_CPUS];

/** The APIC id supplied by the chip model for logical processor `cpu`. */
uint32_t smpboot_apic_id(int cpu)
{
    return model_apic[cpu];
}

/* ---- the log ---- */

static char logbuf[1 << 17];
static unsigned long loglen;

/** Append the record to the log, as `subsystem: text` and a newline. */
void klog(int level, const char *subsystem, const char *format, ...)
{
    va_list ap;
    int n;

    (void)level;
    if (loglen + 1024 > sizeof(logbuf)) {
        return;
    }
    n = snprintf(logbuf + loglen, 64, "%s: ", subsystem);
    loglen += (unsigned long)n;
    va_start(ap, format);
    n = vsnprintf(logbuf + loglen, 900, format, ap);
    va_end(ap);
    loglen += (unsigned long)(n < 900 ? n : 899);
    logbuf[loglen++] = '\n';
    logbuf[loglen] = 0;
}

/** Whether the log has `s`. */
static int logged(const char *s)
{
    return strstr(logbuf, s) != NULL;
}

/** How many times the log has `s`. */
static int log_count(const char *s)
{
    const char *p = logbuf;
    int n = 0;

    while ((p = strstr(p, s)) != NULL) {
        n++;
        p++;
    }
    return n;
}

/* ---- the machine around the driver ---- */

/** No interrupts to hold off on the host. */
uint32_t kirq_save(void)
{
    return 0;
}

/** Nothing to restore. */
void kirq_restore(uint32_t flags)
{
    (void)flags;
}

/* The boot processor's APIC id in the mode it is in, the destination of
 * every message, and its x2APIC id, which the APIC takes as its id when it
 * moves to x2APIC mode; in xAPIC mode the first is the second's low eight
 * bits, as the SDM has them. */
static uint32_t boot_apic, boot_x2apic;

/** The model keeps the whole x2APIC id even before switching modes. */
uint32_t lapic_x2apic_id(void)
{
    return boot_x2apic;
}

/** The boot processor, which every message goes to. */
uint32_t lapic_id(void)
{
    return boot_apic;
}

/** The configuration ports: no bus answers past the ECAM window. */
void outd(uint16_t port, uint32_t value)
{
    (void)port;
    (void)value;
}

/** An absent function reads all ones. */
uint32_t ind(uint16_t port)
{
    (void)port;
    return 0xFFFFFFFFu;
}

/** As `outd`. */
void outw(uint16_t port, uint16_t value)
{
    (void)port;
    (void)value;
}

/** As `ind`. */
uint16_t inw(uint16_t port)
{
    (void)port;
    return 0xFFFF;
}

/** As `outd`. */
void outb(uint16_t port, uint8_t value)
{
    (void)port;
    (void)value;
}

/** As `ind`. */
uint8_t inb(uint16_t port)
{
    (void)port;
    return 0xFF;
}

/* Configuration space of buses 0 to 7, as ECAM lays it out. */
#define ECAM_BUSES 8
static unsigned char *ecam_mem;

/** The configuration space of (bus, devfn). */
static unsigned char *cfg(uint8_t bus, uint8_t devfn)
{
    return ecam_mem + ((unsigned long)bus << 20) + ((unsigned long)devfn << 12);
}

/** The dword at `off` of (bus, devfn). */
static uint32_t cf32(uint8_t bus, uint8_t devfn, unsigned int off)
{
    const unsigned char *c = cfg(bus, devfn) + off;

    return (uint32_t)c[0] | ((uint32_t)c[1] << 8) | ((uint32_t)c[2] << 16) | ((uint32_t)c[3] << 24);
}

/** The halfword at `off` of (bus, devfn). */
static uint16_t cf16(uint8_t bus, uint8_t devfn, unsigned int off)
{
    const unsigned char *c = cfg(bus, devfn) + off;

    return (uint16_t)(c[0] | (c[1] << 8));
}

/** A function at (bus, devfn) of class `cls`, its three bytes, and header
 * type `header`. */
static void cf_dev(uint8_t bus, uint8_t devfn, uint32_t cls, uint8_t header)
{
    unsigned char *c = cfg(bus, devfn);

    memset(c, 0, 4096);
    c[0] = 0x86;
    c[1] = 0x80;
    c[2] = devfn;
    c[3] = bus;
    c[9] = (unsigned char)cls;
    c[10] = (unsigned char)(cls >> 8);
    c[11] = (unsigned char)(cls >> 16);
    c[0x0E] = header;
}

/** Put capability `id` at `at` on the end of (bus, devfn)'s list. */
static void cf_cap(uint8_t bus, uint8_t devfn, uint8_t at, uint8_t id)
{
    unsigned char *c = cfg(bus, devfn);
    unsigned int p = 0x34;

    c[6] |= 0x10;
    while (c[p] != 0) {
        p = c[p] + 1u;
    }
    c[p] = at;
    c[at] = id;
    c[at + 1] = 0;
}

/** A 64-bit MSI capability at 0x50. */
static void cf_msi(uint8_t bus, uint8_t devfn)
{
    cf_cap(bus, devfn, 0x50, PCI_CAP_ID_MSI);
    cfg(bus, devfn)[0x52] = 0x80;
}

/** An MSI-X capability of four entries at 0x70. */
static void cf_msix(uint8_t bus, uint8_t devfn)
{
    cf_cap(bus, devfn, 0x70, PCI_CAP_ID_MSIX);
    cfg(bus, devfn)[0x72] = 3;
}

/** A PCI Express capability of port type `type` at 0x90. */
static void cf_pcie(uint8_t bus, uint8_t devfn, unsigned int type)
{
    cf_cap(bus, devfn, 0x90, PCI_CAP_ID_PCIE);
    cfg(bus, devfn)[0x92] = (unsigned char)(type << 4);
}

/** A bridge to buses sec to sub; `type` is its PCI Express port type, or
 * negative for a conventional PCI bridge. */
static void cf_bridge(uint8_t bus, uint8_t devfn, uint8_t sec, uint8_t sub, int type)
{
    unsigned char *c = cfg(bus, devfn);

    cf_dev(bus, devfn, 0x060400, 0x01);
    c[0x18] = bus;
    c[0x19] = sec;
    c[0x1A] = sub;
    if (type >= 0) {
        cf_pcie(bus, devfn, (unsigned int)type);
    }
}

/** QEMU's q35 with the devices of the boot check: VGA, an e1000, NVMe, the
 * ISA bridge, AHCI and SMBus. The e1000 has MSI here, for the rows. */
static void pci_qemu(void)
{
    cf_dev(0, DF(0, 0), 0x060000, 0);
    cf_dev(0, DF(1, 0), 0x030000, 0);
    cf_dev(0, DF(2, 0), 0x020000, 0);
    cf_msi(0, DF(2, 0));
    cf_dev(0, DF(3, 0), 0x010802, 0);
    cf_msi(0, DF(3, 0));
    cf_msix(0, DF(3, 0));
    cf_dev(0, DF(0x1F, 0), 0x060100, 0x80);
    cf_dev(0, DF(0x1F, 2), 0x010601, 0);
    cf_msi(0, DF(0x1F, 2));
    cf_dev(0, DF(0x1F, 3), 0x0C0500, 0);
}

/** The Lenovo's functions, as Linux lists them, with the AR9565 behind root
 * port 1c.0 and the RTL8168 behind 1c.1. */
static void pci_lenovo(void)
{
    cf_dev(0, DF(0, 0), 0x060000, 0);
    cf_dev(0, DF(2, 0), 0x030000, 0);
    cf_msi(0, DF(2, 0));
    cf_dev(0, DF(3, 0), 0x040300, 0);
    cf_dev(0, DF(0x14, 0), 0x0C0330, 0);
    cf_msi(0, DF(0x14, 0));
    cf_dev(0, DF(0x16, 0), 0x078000, 0);
    cf_dev(0, DF(0x1B, 0), 0x040300, 0);
    cf_bridge(0, DF(0x1C, 0), 1, 1, 4);
    cf_bridge(0, DF(0x1C, 1), 2, 2, 4);
    cf_dev(0, DF(0x1D, 0), 0x0C0320, 0);
    cf_dev(0, DF(0x1F, 0), 0x060100, 0x80);
    cf_dev(0, DF(0x1F, 2), 0x010601, 0);
    cf_msi(0, DF(0x1F, 2));
    cf_dev(0, DF(0x1F, 3), 0x0C0500, 0);
    cf_dev(1, DF(0, 0), 0x028000, 0);
    cf_msi(1, DF(0, 0));
    cf_pcie(1, DF(0, 0), 0);
    cf_dev(2, DF(0, 0), 0x020000, 0);
    cf_msi(2, DF(0, 0));
    cf_msix(2, DF(0, 0));
    cf_pcie(2, DF(0, 0), 0);
}

/** A device handle for (bus, devfn). */
static pci_device_t t_dev(uint8_t bus, uint8_t devfn)
{
    pci_device_t d;

    memset(&d, 0, sizeof(d));
    d.bus = bus;
    d.device = (uint8_t)(devfn >> 3);
    d.function = (uint8_t)(devfn & 7u);
    return d;
}

/* The devices a row gave messages, which its end takes back; `msix` is
 * the number of the MSI-X table plus one, or 0 for MSI. */
static struct {
    uint8_t bus, devfn;
    int msix;
} sent[16];
static int nsent;
static uint32_t msix_tables[4][16];

/** Enable MSI on (bus, devfn) for `vector` and remember it. */
static int t_msi(uint8_t bus, uint8_t devfn, uint8_t vector)
{
    pci_device_t d = t_dev(bus, devfn);

    sent[nsent].bus = bus;
    sent[nsent].devfn = devfn;
    sent[nsent++].msix = 0;
    return pci_msi_enable(&d, vector);
}

/** Enable MSI-X entry 0 of (bus, devfn), whose table is `msix_tables[k]`,
 * for `vector`, and remember it. */
static int t_msix(uint8_t bus, uint8_t devfn, int k, uint8_t vector)
{
    pci_device_t d = t_dev(bus, devfn);

    sent[nsent].bus = bus;
    sent[nsent].devfn = devfn;
    sent[nsent++].msix = k + 1;
    return pci_msix_enable(&d, (uintptr_t)msix_tables[k], vector);
}

/* ---- pages, and what a unit that does not snoop sees of them ---- */

struct mpage {
    struct mpage *next;
    uint64_t phys;
    unsigned char *seen; /* the page as memory holds it, for a unit that does
                            not snoop the processor's caches */
};

#define MPAGE_HASH 8192
static struct mpage *mpages[MPAGE_HASH];
static long mpage_count;
static long page_fail_after = -1; /* pages given before every later one fails */
static unsigned long flush_calls;
static unsigned long stray_flushes;

/** The record of the page holding `phys`, or NULL. */
static struct mpage *mpage_find(uint64_t phys)
{
    struct mpage *m;

    phys &= ~0xFFFull;
    for (m = mpages[(phys >> 12) % MPAGE_HASH]; m != NULL; m = m->next) {
        if (m->phys == phys) {
            return m;
        }
    }
    return NULL;
}

/** A zeroed page, whose copy in memory is stale bytes until flushed. */
void *vtd_hw_page(uint64_t *phys)
{
    unsigned char *p;
    struct mpage *m;

    if (page_fail_after == 0) {
        return NULL;
    }
    if (page_fail_after > 0) {
        page_fail_after--;
    }
    p = (unsigned char *)aligned_alloc(4096, 4096);
    m = (struct mpage *)calloc(1, sizeof(*m));
    m->seen = (unsigned char *)aligned_alloc(4096, 4096);
    memset(p, 0, 4096);
    memset(m->seen, 0xA5, 4096);
    m->phys = (uint64_t)(uintptr_t)p;
    m->next = mpages[(m->phys >> 12) % MPAGE_HASH];
    mpages[(m->phys >> 12) % MPAGE_HASH] = m;
    mpage_count++;
    *phys = m->phys;
    return p;
}

/** Free a page `vtd_hw_page` gave. */
void vtd_hw_page_free(void *va)
{
    uint64_t phys = (uint64_t)(uintptr_t)va;
    struct mpage **pp = &mpages[(phys >> 12) % MPAGE_HASH];

    while (*pp != NULL && (*pp)->phys != phys) {
        pp = &(*pp)->next;
    }
    if (*pp != NULL) {
        struct mpage *m = *pp;

        *pp = m->next;
        free(m->seen);
        free(m);
        mpage_count--;
    }
    free(va);
}

/** Physical addresses are the host's own. */
void *vtd_hw_virt(uint64_t phys)
{
    return (void *)(uintptr_t)phys;
}

/** Copy the lines of [p, p + len) to memory, where a unit that does not
 * snoop reads them. */
void vtd_hw_flush(const void *p, unsigned long len)
{
    uint64_t a = (uint64_t)(uintptr_t)p & ~63ull, end = (uint64_t)(uintptr_t)p + len;

    flush_calls++;
    for (; a < end; a += 64) {
        struct mpage *m = mpage_find(a);

        if (m == NULL) {
            stray_flushes++;
            continue;
        }
        memcpy(m->seen + (a & 0xFFFu), (const void *)(uintptr_t)a, 64);
    }
}

/* ---- the units ---- */

#define M_CC  16
#define M_TLB 64

struct model {
    uint64_t base;
    uint64_t cap, ecap;
    uint32_t regs[1024];
    uint32_t gsts;
    uint32_t cmd;    /* the persistent command bits last written */
    int cfi_quiet;   /* GSTS.CFIS never follows, as on QEMU */
    int refuse_next; /* the next descriptor is refused (IQE) */
    uint64_t root;   /* the root table, latched by SRTP */
    uint64_t irta;   /* the remapping table, latched by SIRTP */
    int root_set, irta_set;
    int owe_cc, owe_tlb, owe_iec; /* invalidations a new pointer needs */
    struct {
        int valid;
        uint16_t sid;
        uint64_t lo, hi;
    } cc[M_CC];
    struct {
        int valid, level;
        uint16_t did;
        uint64_t page, leaf;
    } tlb[M_TLB];
    struct {
        int valid;
        uint64_t lo, hi;
    } iec[VTD_IRT_ENTRIES];
    unsigned int cc_next, tlb_next, fault_next;
    unsigned int refill; /* faults recorded again as a record is cleared,
                            as by a device that keeps faulting */
    unsigned int last_dest, last_level;
    unsigned long wbf, inv_cc, inv_tlb, inv_iec, waits, iqe;
    unsigned long writes;     /* register writes of every kind */
    int fectl_posted;         /* a FECTL write no read of the unit has flushed */
    unsigned long violations; /* commands the specification forbids */
    unsigned long stray;      /* reads of memory no table is in */
};

static struct model models[4];
static int nmodels;

/** The model of unit `i` of the driver. */
static struct model *M(int i)
{
    return (struct model *)(void *)vtd_units[i].regs;
}

/** A unit at `base` with registers `cap` and `ecap`, version 1.0. */
static struct model *m_add(uint64_t base, uint64_t cap, uint64_t ecap)
{
    struct model *m = &models[nmodels++];

    memset(m, 0, sizeof(*m));
    m->base = base;
    m->cap = cap;
    m->ecap = ecap;
    m->regs[0] = 0x10;
    m->regs[VTD_CAP / 4] = (uint32_t)cap;
    m->regs[VTD_CAP / 4 + 1] = (uint32_t)(cap >> 32);
    m->regs[VTD_ECAP / 4] = (uint32_t)ecap;
    m->regs[VTD_ECAP / 4 + 1] = (uint32_t)(ecap >> 32);
    return m;
}

/** The 64-bit register at `off`. */
static uint64_t r64(const struct model *m, unsigned int off)
{
    return (uint64_t)m->regs[off / 4] | ((uint64_t)m->regs[off / 4 + 1] << 32);
}

/** Store the 64-bit register at `off`. */
static void w64(struct model *m, unsigned int off, uint64_t v)
{
    m->regs[off / 4] = (uint32_t)v;
    m->regs[off / 4 + 1] = (uint32_t)(v >> 32);
}

/** Read the quadword at `phys` as the unit sees memory. */
static uint64_t m_read(struct model *m, uint64_t phys)
{
    struct mpage *pg = mpage_find(phys);
    uint64_t v;

    if (pg == NULL) {
        m->stray++;
        return 0;
    }
    if (VTD_ECAP_C(m->ecap)) {
        return *(const uint64_t *)(uintptr_t)phys;
    }
    memcpy(&v, pg->seen + (phys & 0xFF8u), 8);
    return v;
}

/** The fault status register: PPF and the index of the first full record
 * come from the records. */
static uint32_t m_fsts(const struct model *m)
{
    unsigned int fro = VTD_CAP_FRO(m->cap), nfr = VTD_CAP_NFR(m->cap), i;
    uint32_t s = m->regs[VTD_FSTS / 4] & ~0xFF02u;

    for (i = 0; i < nfr; i++) {
        if ((m->regs[(fro + 16u * i + 12u) / 4] & 0x80000000u) != 0) {
            return s | VTD_FSTS_PPF | (i << 8);
        }
    }
    return s;
}

/** Record a fault of `reason` from `sid`, with `info` the page or the
 * interrupt index in bits 63:48, or set PFO when the record is full. */
static int m_fault(struct model *m, uint16_t sid, uint64_t info, int read, unsigned int reason)
{
    unsigned int fro = VTD_CAP_FRO(m->cap), nfr = VTD_CAP_NFR(m->cap);
    uint32_t *rec = &m->regs[(fro + 16u * (m->fault_next % nfr)) / 4];

    if ((rec[3] & 0x80000000u) != 0) {
        m->regs[VTD_FSTS / 4] |= VTD_FSTS_PFO;
        return (int)reason;
    }
    rec[0] = (uint32_t)info;
    rec[1] = (uint32_t)(info >> 32);
    rec[2] = sid;
    rec[3] = reason | (read ? 1u << 30 : 0u) | 0x80000000u;
    m->fault_next++;
    return (int)reason;
}

/** Context-cache invalidation of granularity `g`: 1 all, 2 a domain, 3 a
 * device. */
static void m_inv_cc(struct model *m, unsigned int g, unsigned int sid, unsigned int did)
{
    int i;

    for (i = 0; i < M_CC; i++) {
        if (g == 1 || (g == 2 && ((m->cc[i].hi >> 8) & 0xFFFFu) == did) ||
            (g == 3 && m->cc[i].sid == sid)) {
            m->cc[i].valid = 0;
        }
    }
    if (g == 1) {
        m->owe_cc = 0;
    }
    m->inv_cc++;
}

/** IOTLB invalidation of granularity `g`: 1 all, 2 a domain. */
static void m_inv_tlb(struct model *m, unsigned int g, unsigned int did)
{
    int i;

    for (i = 0; i < M_TLB; i++) {
        if (g == 1 || m->tlb[i].did == did) {
            m->tlb[i].valid = 0;
        }
    }
    if (g == 1) {
        m->owe_tlb = 0;
    }
    m->inv_tlb++;
}

static int m_points_at(const struct model *m, unsigned int index);

/** Interrupt-entry cache invalidation: all of it, or entry `index`. An
 * entry cleared while a device or the I/O APIC still names it would refuse
 * that source's next interrupt. */
static void m_inv_iec(struct model *m, int global, unsigned int index)
{
    unsigned int i;

    if (!global && m->irta_set &&
        (*(const uint64_t *)(uintptr_t)((m->irta & ~0xFFFull) + 16u * index) & 1u) == 0 &&
        m_points_at(m, index)) {
        m->violations++;
    }

    for (i = 0; i < VTD_IRT_ENTRIES; i++) {
        if (global || i == index) {
            m->iec[i].valid = 0;
        }
    }
    if (global) {
        m->owe_iec = 0;
    }
    m->inv_iec++;
}

/** Carry out descriptor (lo, hi): 1, or 0 for one the unit refuses. */
static int m_desc(struct model *m, uint64_t lo, uint64_t hi)
{
    switch ((unsigned int)(lo & 0xFu)) {
    case 1:
        m_inv_cc(m, (unsigned int)(lo >> 4) & 3u, (unsigned int)(lo >> 32) & 0xFFFFu,
                 (unsigned int)(lo >> 16) & 0xFFFFu);
        return 1;
    case 2:
        m_inv_tlb(m, (unsigned int)(lo >> 4) & 3u, (unsigned int)(lo >> 16) & 0xFFFFu);
        return 1;
    case 4:
        m_inv_iec(m, ((lo >> 4) & 1u) == 0, (unsigned int)(lo >> 32) & 0xFFFFu);
        return 1;
    case 5:
        if ((lo & (1u << 5)) != 0) {
            *(volatile uint32_t *)(uintptr_t)hi = (uint32_t)(lo >> 32);
        }
        m->waits++;
        return 1;
    default:
        return 0;
    }
}

/** Fetch and carry out descriptors from IQH to IQT, stopping at a refused
 * one with IQE set and IQH on it. */
static void m_queue(struct model *m)
{
    uint64_t iqa = r64(m, VTD_IQA) & ~0xFFFull;
    unsigned int head = (unsigned int)(r64(m, VTD_IQH) >> 4) & 0xFFu;
    unsigned int tail = (unsigned int)(r64(m, VTD_IQT) >> 4) & 0xFFu;

    if ((m->regs[VTD_FSTS / 4] & VTD_FSTS_IQE) != 0) {
        return;
    }
    while (head != tail) {
        const uint64_t *d = (const uint64_t *)(uintptr_t)(iqa + 16u * head);

        if (m->refuse_next || !m_desc(m, d[0], d[1])) {
            m->refuse_next = 0;
            m->iqe++;
            m->regs[VTD_FSTS / 4] |= VTD_FSTS_IQE;
            break;
        }
        head = (head + 1u) % 256u;
    }
    w64(m, VTD_IQH, (uint64_t)head << 4);
}

/** How many bits of `v` are set. */
static int bits(uint32_t v)
{
    int n = 0;

    for (; v != 0; v &= v - 1u) {
        n++;
    }
    return n;
}

static int m_compat_live(const struct model *m);

/** A write of the global command register. Remapping on with the
 * compatibility format refused while a source still sends in it is a
 * violation: that source's interrupts would be lost. */
static void m_gcmd(struct model *m, uint32_t v)
{
    uint32_t persist = VTD_GCMD_TE | VTD_GCMD_QIE | VTD_GCMD_IRE | VTD_GCMD_CFI;
    uint32_t once = v & (VTD_GCMD_SRTP | VTD_GCMD_SIRTP | VTD_GCMD_WBF);
    uint32_t changed = (v ^ m->cmd) & persist;

    if (bits(changed) + bits(once) > 1) {
        m->violations++;
    }
    if ((once & VTD_GCMD_SRTP) != 0) {
        m->root = r64(m, VTD_RTADDR) & ~0xFFFull;
        m->root_set = 1;
        m->owe_cc = m->owe_tlb = 1;
        m->gsts |= VTD_GCMD_SRTP;
    }
    if ((once & VTD_GCMD_SIRTP) != 0) {
        m->irta = r64(m, VTD_IRTA);
        m->irta_set = 1;
        m->owe_iec = 1;
        m->gsts |= VTD_GCMD_SIRTP;
    }
    m->wbf += (once & VTD_GCMD_WBF) != 0;
    if ((changed & v & VTD_GCMD_TE) != 0 && (!m->root_set || m->owe_cc || m->owe_tlb)) {
        m->violations++;
    }
    if ((changed & VTD_GCMD_QIE) != 0) {
        if ((v & VTD_GCMD_QIE) != 0) {
            w64(m, VTD_IQH, 0);
        } else if (r64(m, VTD_IQH) != r64(m, VTD_IQT)) {
            m->violations++;
        }
    }
    if ((changed & v & VTD_GCMD_IRE) != 0 && (!m->irta_set || m->owe_iec)) {
        m->violations++;
    }
    m->cmd = v & persist;
    m->gsts = (m->gsts & ~persist) | m->cmd;
    if (m->cfi_quiet) {
        m->gsts &= ~VTD_GCMD_CFI;
    }
    if ((v & (VTD_GCMD_IRE | VTD_GCMD_CFI)) == VTD_GCMD_IRE && m_compat_live(m)) {
        m->violations++;
    }
}

/** The unit at `base`, mapped: the model is its own handle. */
uintptr_t vtd_hw_map(uint64_t base)
{
    int i;

    for (i = 0; i < nmodels; i++) {
        if (models[i].base == base) {
            return (uintptr_t)(void *)&models[i];
        }
    }
    return 0;
}

/** A 32-bit register: status registers are computed, the rest stored. A
 * read flushes the unit's posted writes. */
uint32_t vtd_hw_read32(uintptr_t regs, unsigned int off)
{
    struct model *m = (struct model *)(void *)regs;

    m->fectl_posted = 0;
    if (off == VTD_GSTS) {
        return m->gsts;
    }
    if (off == VTD_FSTS) {
        return m_fsts(m);
    }
    return m->regs[off / 4];
}

/** A 64-bit register, as stored. */
uint64_t vtd_hw_read64(uintptr_t regs, unsigned int off)
{
    return r64((const struct model *)(void *)regs, off);
}

/** A 32-bit write: the command register, the status bits a write of one
 * clears, a fault record's F bit, PMEN, or a stored value. */
void vtd_hw_write32(uintptr_t regs, unsigned int off, uint32_t v)
{
    struct model *m = (struct model *)(void *)regs;
    unsigned int fro = VTD_CAP_FRO(m->cap), nfr = VTD_CAP_NFR(m->cap);

    m->writes++;
    if (off == VTD_GCMD) {
        m_gcmd(m, v);
    } else if (off == VTD_FSTS) {
        m->regs[off / 4] &= ~(v & 0xF1u);
        if ((v & VTD_FSTS_IQE) != 0 && (m->cmd & VTD_GCMD_QIE) != 0) {
            m_queue(m);
        }
    } else if (off >= fro && off < fro + 16u * nfr && (off - fro) % 16u == 12u) {
        uint32_t *rec = &m->regs[(off - 12u) / 4];

        m->regs[off / 4] &= ~(v & 0x80000000u);
        if (m->refill > 0 && (rec[3] & 0x80000000u) == 0) {
            m->refill--;
            m->fault_next = (off - fro) / 16u;
            (void)m_fault(m, (uint16_t)rec[2], ((uint64_t)rec[1] << 32 | rec[0]) + 0x1000u,
                          (int)((rec[3] >> 30) & 1u), rec[3] & 0xFFu);
        }
    } else if (off == VTD_PMEN) {
        m->regs[off / 4] = (v & 0x80000000u) | (v >> 31);
    } else {
        m->regs[off / 4] = v;
        m->fectl_posted = m->fectl_posted || off == VTD_FECTL;
    }
}

/** A 64-bit write: the queue's tail, the register-based invalidations, or
 * a stored value. */
void vtd_hw_write64(uintptr_t regs, unsigned int off, uint64_t v)
{
    struct model *m = (struct model *)(void *)regs;

    m->writes++;
    if (off == VTD_CCMD && (v >> 63) != 0) {
        m_inv_cc(m, (unsigned int)(v >> 61) & 3u, (unsigned int)(v >> 16) & 0xFFFFu,
                 (unsigned int)v & 0xFFFFu);
        v &= ~(1ull << 63);
    } else if (off == VTD_ECAP_IRO(m->ecap) + 8u && (v >> 63) != 0) {
        m_inv_tlb(m, (unsigned int)(v >> 60) & 3u, (unsigned int)(v >> 32) & 0xFFFFu);
        v &= ~(1ull << 63);
    }
    w64(m, off, v);
    if (off == VTD_IQT && (m->cmd & VTD_GCMD_QIE) != 0) {
        m_queue(m);
    }
}

/** The context entry of `sid`, from the cache or the tables: 0, or the
 * fault reason when it is not present. */
static unsigned int m_context(struct model *m, uint16_t sid, uint64_t *lo, uint64_t *hi)
{
    uint64_t root;
    int i;

    for (i = 0; i < M_CC; i++) {
        if (m->cc[i].valid && m->cc[i].sid == sid) {
            *lo = m->cc[i].lo;
            *hi = m->cc[i].hi;
            return (*lo & 1u) ? 0u : 2u;
        }
    }
    root = m_read(m, m->root + 16ull * (sid >> 8));
    *lo = *hi = 0;
    if ((root & 1u) != 0) {
        *lo = m_read(m, (root & VTD_PTE_ADDR) + 16ull * (sid & 0xFFu));
        *hi = m_read(m, (root & VTD_PTE_ADDR) + 16ull * (sid & 0xFFu) + 8u);
    }
    if ((*lo & 1u) != 0 || VTD_CAP_CM(m->cap)) {
        i = (int)(m->cc_next++ % M_CC);
        m->cc[i].valid = 1;
        m->cc[i].sid = sid;
        m->cc[i].lo = *lo;
        m->cc[i].hi = *hi;
    }
    if ((root & 1u) == 0) {
        return 1;
    }
    return (*lo & 1u) ? 0u : 2u;
}

/** Walk `levels` levels from `table` for `iova`: the leaf, or 0, with its
 * level in `*level`; -1 for a reserved bit set. */
static int m_walk(struct model *m, uint64_t table, int levels, uint64_t iova, uint64_t *leaf,
                  int *level)
{
    int l;

    for (l = levels; l >= 1; l--) {
        uint64_t e = m_read(m, table + 8u * ((iova >> (12 + 9 * (l - 1))) & 511u));

        if ((e & (VTD_PTE_R | VTD_PTE_W)) == 0) {
            break;
        }
        if (l == 1 || (e & VTD_PTE_PS) != 0) {
            if ((l > 1 && (VTD_CAP_SLLPS(m->cap) & (1u << (l - 2))) == 0) ||
                ((e & VTD_PTE_SNP) != 0 && !VTD_ECAP_SC(m->ecap))) {
                return -1;
            }
            *leaf = e;
            *level = l;
            return 0;
        }
        table = e & VTD_PTE_ADDR;
    }
    *leaf = 0;
    *level = 1;
    return 0;
}

/** The leaf for `iova` under context entry (lo, hi), from the IOTLB or a
 * walk: 0, or -1 for a reserved bit. */
static int m_translate(struct model *m, uint64_t lo, uint64_t hi, uint64_t iova, uint64_t *leaf,
                       int *level)
{
    unsigned int did = (unsigned int)(hi >> 8) & 0xFFFFu;
    int i;

    for (i = 0; i < M_TLB; i++) {
        if (m->tlb[i].valid && m->tlb[i].did == did && m->tlb[i].page == iova >> 12) {
            *leaf = m->tlb[i].leaf;
            *level = m->tlb[i].level;
            return 0;
        }
    }
    if (m_walk(m, lo & VTD_PTE_ADDR, (int)(hi & 7u) + 2, iova, leaf, level) != 0) {
        return -1;
    }
    if (*leaf != 0 || VTD_CAP_CM(m->cap)) {
        i = (int)(m->tlb_next++ % M_TLB);
        m->tlb[i].valid = 1;
        m->tlb[i].did = (uint16_t)did;
        m->tlb[i].page = iova >> 12;
        m->tlb[i].leaf = *leaf;
        m->tlb[i].level = *level;
    }
    return 0;
}

/** A DMA request of (bus, devfn) to `iova`: 0 with the address it reached
 * in `*phys`, or the reason of the fault the unit recorded. */
static int m_dma(struct model *m, uint8_t bus, uint8_t devfn, uint64_t iova, int write,
                 uint64_t *phys)
{
    uint16_t sid = (uint16_t)((bus << 8) | devfn);
    uint64_t lo, hi, leaf, span;
    unsigned int tt, aw, r;
    int level;

    if ((m->gsts & VTD_GCMD_TE) == 0) {
        *phys = iova;
        return 0;
    }
    if ((r = m_context(m, sid, &lo, &hi)) != 0) {
        return m_fault(m, sid, iova & ~0xFFFull, !write, r);
    }
    tt = (unsigned int)(lo >> 2) & 3u;
    aw = (unsigned int)hi & 7u;
    if (tt == VTD_TT_PASSTHROUGH && VTD_ECAP_PT(m->ecap)) {
        *phys = iova;
        return 0;
    }
    if (tt != VTD_TT_TRANSLATE || (VTD_CAP_SAGAW(m->cap) & (1u << aw)) == 0) {
        return m_fault(m, sid, iova & ~0xFFFull, !write, 3);
    }
    if ((iova >> VTD_CAP_MGAW(m->cap)) != 0) {
        return m_fault(m, sid, iova & ~0xFFFull, !write, 4);
    }
    if (m_translate(m, lo, hi, iova, &leaf, &level) != 0) {
        return m_fault(m, sid, iova & ~0xFFFull, !write, 0xC);
    }
    if ((leaf & (write ? VTD_PTE_W : VTD_PTE_R)) == 0) {
        return m_fault(m, sid, iova & ~0xFFFull, !write, write ? 5u : 6u);
    }
    span = 1ull << (12 + 9 * (level - 1));
    *phys = (leaf & VTD_PTE_ADDR & ~(span - 1u)) | (iova & (span - 1u));
    return 0;
}

/** An interrupt from `sid`: in the remappable format, entry `index`;
 * otherwise `compat`, a vector. The vector delivered, or -1 with a fault
 * recorded. */
static int m_irq(struct model *m, uint16_t sid, int remappable, unsigned int index,
                 unsigned int compat)
{
    uint64_t lo, hi;
    unsigned int svt, s;

    if ((m->gsts & VTD_GCMD_IRE) == 0) {
        return (int)compat;
    }
    if (!remappable) {
        if ((m->cmd & VTD_GCMD_CFI) != 0) {
            return (int)compat;
        }
        (void)m_fault(m, sid, 0, 0, 0x25);
        return -1;
    }
    if (index >= (2u << (m->irta & 0xFu))) {
        (void)m_fault(m, sid, (uint64_t)index << 48, 0, 0x21);
        return -1;
    }
    if (!m->iec[index].valid) {
        m->iec[index].lo = m_read(m, (m->irta & ~0xFFFull) + 16u * index);
        m->iec[index].hi = m_read(m, (m->irta & ~0xFFFull) + 16u * index + 8u);
        m->iec[index].valid = 1;
    }
    lo = m->iec[index].lo;
    hi = m->iec[index].hi;
    if ((lo & 1u) == 0) {
        (void)m_fault(m, sid, (uint64_t)index << 48, 0, 0x22);
        return -1;
    }
    svt = (unsigned int)(hi >> 18) & 3u;
    s = (unsigned int)hi & 0xFFFFu;
    if ((svt == 1 && ((hi >> 16) & 3u) == 0 && s != sid) ||
        (svt == 2 && ((sid >> 8) < (s >> 8) || (sid >> 8) > (s & 0xFFu)))) {
        (void)m_fault(m, sid, (uint64_t)index << 48, 0, 0x26);
        return -1;
    }
    m->last_dest =
        (m->irta & (1u << 11)) != 0 ? (unsigned int)(lo >> 32) : (unsigned int)(lo >> 40) & 0xFFu;
    m->last_level = (unsigned int)(lo >> 4) & 1u;
    return (int)((lo >> 16) & 0xFFu);
}

/** A message (address, data) from `sid`. */
static int m_msi(struct model *m, uint16_t sid, uint32_t address, uint32_t data)
{
    unsigned int index = ((address >> 5) & 0x7FFFu) | (((address >> 2) & 1u) << 15);

    if ((address & (1u << 3)) != 0) {
        index += data & 0xFFFFu;
    }
    return m_irq(m, sid, (address & (1u << 4)) != 0, index, data & 0xFFu);
}

/** An I/O APIC's interrupt of redirection entry (low, high) from `sid`. */
static int m_rte(struct model *m, uint16_t sid, uint32_t low, uint32_t high)
{
    unsigned int index = (high >> 17) | (((low >> 11) & 1u) << 15);

    return m_irq(m, sid, (high & (1u << 16)) != 0, index, low & 0xFFu);
}

/* ---- the rest of hw.c's interface ---- */

static uint64_t ram_top;
static const uint8_t *dmar_table;
static uint32_t dmar_len;
static const char *opt_intel_iommu; /* the boot line's values, or NULL */
static const char *opt_intremap;
static int opt_noapic; /* noapic or maxcpus=0 */
static uint8_t ioapic_ids[4];
static int nioapic_ids;

/** Nothing to wait for: the model answers at once. */
void vtd_hw_udelay(unsigned int us)
{
    (void)us;
}

/** As hw.c reads it. */
uint32_t vtd_hw_class(uint8_t bus, uint8_t devfn)
{
    uint8_t dev = (uint8_t)(devfn >> 3), fn = (uint8_t)(devfn & 7u);

    if (pci_read_config16(bus, dev, fn, PCI_VENDOR_ID) == 0xFFFF) {
        return 0xFFFFFFFFu;
    }
    return pci_read_config32(bus, dev, fn, 0x08) >> 8;
}

/** As hw.c reads it. */
int vtd_hw_bridge(uint8_t bus, uint8_t devfn, uint8_t *sec, uint8_t *sub)
{
    uint8_t dev = (uint8_t)(devfn >> 3), fn = (uint8_t)(devfn & 7u);

    if (pci_read_config16(bus, dev, fn, PCI_VENDOR_ID) == 0xFFFF ||
        (pci_read_config8(bus, dev, fn, PCI_HEADER_TYPE) & 0x7Fu) != 0x01u) {
        return -1;
    }
    *sec = pci_read_config8(bus, dev, fn, 0x19);
    *sub = pci_read_config8(bus, dev, fn, 0x1A);
    return 0;
}

/** As hw.c reads it. */
int vtd_hw_bus_present(uint8_t bus)
{
    uint8_t dev;

    for (dev = 0; dev < 32; dev++) {
        if (pci_read_config16(bus, dev, 0, PCI_VENDOR_ID) != 0xFFFF) {
            return 1;
        }
    }
    return 0;
}

/** The row's end of RAM. */
uint64_t vtd_hw_ram_top(void)
{
    return ram_top;
}

/** The row's table. */
const uint8_t *vtd_hw_dmar(uint32_t *len)
{
    *len = dmar_len;
    return dmar_table;
}

/** Whether the row's value `p`, a list separated by commas, has `value`;
 * cmdline.c's `kcmdline_list` and its rows in ioapic_hosttest.c read the
 * boot line itself. */
static int opt_has(const char *p, const char *value)
{
    unsigned long n = 0, i;

    while (value[n] != 0) {
        n++;
    }
    while (p != NULL && *p != 0) {
        unsigned long len = 0;

        while (p[len] != 0 && p[len] != ',') {
            len++;
        }
        for (i = 0; len == n && i < n && p[i] == value[i]; i++) {
        }
        if (len == n && i == n) {
            return 1;
        }
        p += p[len] == ',' ? len + 1 : len;
    }
    return 0;
}

/** Whether the row turns translation off. */
int vtd_hw_dma_off(void)
{
    return opt_has(opt_intel_iommu, "off");
}

/** Whether the row turns remapping off. */
int vtd_hw_intremap_off(void)
{
    return opt_has(opt_intremap, "off");
}

/** Whether the row takes the x2APIC format over the firmware's opt-out. */
int vtd_hw_no_x2apic_optout(void)
{
    return opt_has(opt_intremap, "no_x2apic_optout");
}

/** Whether the row's boot line has noapic or maxcpus=0. */
int vtd_hw_ioapic_off(void)
{
    return opt_noapic;
}

/** The row's I/O APICs. */
int vtd_hw_ioapics(uint8_t *ids, int max)
{
    int i;

    for (i = 0; i < nioapic_ids && i < max; i++) {
        ids[i] = ioapic_ids[i];
    }
    return i;
}

/* The vector the fault event takes, or -1 for none free. */
static int fault_vec;

/** A vector for the fault event; the rows call its handler themselves. */
int vtd_hw_fault_vector(void)
{
    return fault_vec;
}

/* The clock of fault.c's rate limit, in milliseconds. Each row starts a
 * minute on, so that no row shares a window with the one before it. */
static uint32_t now_ms;

/** The model's clock. */
uint32_t vtd_hw_ms(void)
{
    return now_ms;
}

/** The boot processor. */
uint32_t vtd_hw_apic_id(void)
{
    return boot_apic;
}

/** The boot processor's x2APIC id. */
uint32_t vtd_hw_x2apic_id(void)
{
    return boot_x2apic;
}

/* The local APIC's mode: whether it may enter x2APIC mode, whether it is
 * there, and its moves there. Whether the 8259 and the I/O APIC's lines are
 * masked with interrupts off (`vtd_hw_lines_mask`), and, at the last move,
 * whether they were and whether any unit still let the compatibility format
 * by. Nothing here moves the APIC back through the disabled state, which
 * would drop what its request register holds: the vtd interface has no
 * such move, and the rows check that a failed start makes none. */
static int x2_possible, x2_on, x2_moves;
static int lines_masked, x2_moved_masked, x2_moved_cfi;

/** Whether the APIC may be or is in x2APIC mode. */
int vtd_hw_x2apic_possible(void)
{
    return x2_possible || x2_on;
}

/** Move it to x2APIC mode, as lapic.c answers. */
int vtd_hw_x2apic_enable(void)
{
    if (!vtd_hw_x2apic_possible()) {
        return -1;
    }
    if (x2_on) {
        return 0;
    }
    x2_on = 1;
    x2_moves++;
    boot_apic = boot_x2apic;
    x2_moved_masked = lines_masked;
    x2_moved_cfi =
        (M(0)->gsts & VTD_GCMD_CFI) != 0 || (nmodels > 1 && (M(1)->gsts & VTD_GCMD_CFI) != 0);
    return 1;
}

/** Mask the lines, as hw.c does: counted, no line is modelled. */
void vtd_hw_lines_mask(struct vtd_lines *held)
{
    held->flags = 0;
    held->pic = 0;
    held->ioapic = 0;
    lines_masked = 1;
}

/** Unmask them. */
void vtd_hw_lines_restore(const struct vtd_lines *held)
{
    (void)held;
    lines_masked = 0;
}

/* The I/O APIC's routed entries, as ioapic.c keeps them. */
static struct {
    uint8_t id, dest;
    uint32_t low, high;
    int handle;
} rtes[4];
static int nrtes;
static const struct ioapic_remap *rte_remap;

/** Route an entry of I/O APIC `id` with `low` to processor 0. */
static void t_rte(uint8_t id, uint32_t low)
{
    rtes[nrtes].id = id;
    rtes[nrtes].dest = 0;
    rtes[nrtes].low = low;
    rtes[nrtes].high = 0;
    rtes[nrtes++].handle = -1;
}

/** As ioapic.c: each routed entry given an entry of the remapper, or all
 * put back when one cannot be. */
int ioapic_remap_start(const struct ioapic_remap *ops)
{
    int i, j;

    for (i = 0; i < nrtes; i++) {
        uint32_t high;

        rtes[i].handle = ops->map(rtes[i].id, rtes[i].low, rtes[i].high, &high);
        if (rtes[i].handle < 0) {
            for (j = 0; j < i; j++) {
                rtes[j].high = (uint32_t)rtes[j].dest << 24;
                ops->unmap(rtes[j].handle);
                rtes[j].handle = -1;
            }
            return -1;
        }
        rtes[i].high = high;
    }
    rte_remap = ops;
    return 0;
}

/** As ioapic.c: each entry back in the compatibility format. */
void ioapic_remap_stop(void)
{
    int i;

    for (i = 0; i < nrtes; i++) {
        if (rtes[i].handle >= 0 && rte_remap != NULL) {
            rtes[i].high = (uint32_t)rtes[i].dest << 24;
            rte_remap->unmap(rtes[i].handle);
            rtes[i].handle = -1;
        }
    }
    rte_remap = NULL;
}

/** The index of the remappable message (address, data), or -1. */
static int msg_index(uint32_t address)
{
    if ((address & 0x10u) == 0) {
        return -1;
    }
    return (int)(((address >> 5) & 0x7FFFu) | (((address >> 2) & 1u) << 15));
}

/** Whether `m` would refuse what a message of `address` names, when it is
 * `index` or in the compatibility format. */
static int m_names(const struct model *m, uint32_t address, unsigned int index)
{
    if (msg_index(address) < 0) {
        return (m->gsts & VTD_GCMD_IRE) != 0 && (m->cmd & VTD_GCMD_CFI) == 0;
    }
    return msg_index(address) == (int)index;
}

/** Whether a source that can still send would be refused once entry `index`
 * of `m`'s table is cleared: an enabled MSI, or an enabled and unmasked
 * MSI-X entry, that names it or sends the compatibility format `m`
 * refuses, or a remapped redirection entry that names it. */
static int m_points_at(const struct model *m, unsigned int index)
{
    int i, ui;

    for (i = 0; i < nsent; i++) {
        uint8_t b = sent[i].bus, df = sent[i].devfn;

        ui = dmar_unit_for(&vtd_dmar, b, df);
        if (ui < 0 || ui >= vtd_nunits || M(ui) != m) {
            continue;
        }
        if (!sent[i].msix && (cf16(b, df, 0x52) & 1u) != 0 &&
            m_names(m, cf32(b, df, 0x54), index)) {
            return 1;
        }
        if (sent[i].msix && (cf16(b, df, 0x72) & 0xC000u) == 0x8000u &&
            m_names(m, msix_tables[sent[i].msix - 1][0], index)) {
            return 1;
        }
    }
    for (i = 0; i < nrtes; i++) {
        uint16_t sid;

        if (dmar_ioapic_source(&vtd_dmar, rtes[i].id, &ui, &sid) == 0 && M(ui) == m &&
            (rtes[i].high & 0x10000u) != 0 && (rtes[i].high >> 17) == index) {
            return 1;
        }
    }
    return 0;
}

/** Whether a source of `m` sends in the compatibility format: an enabled
 * MSI or unmasked MSI-X entry, or a redirection entry. */
static int m_compat_live(const struct model *m)
{
    int i, ui;

    for (i = 0; i < nsent; i++) {
        uint8_t b = sent[i].bus, df = sent[i].devfn;

        ui = dmar_unit_for(&vtd_dmar, b, df);
        if (ui < 0 || ui >= vtd_nunits || M(ui) != m) {
            continue;
        }
        if ((!sent[i].msix && (cf16(b, df, 0x52) & 1u) != 0 && msg_index(cf32(b, df, 0x54)) < 0) ||
            (sent[i].msix && (cf16(b, df, 0x72) & 0xC000u) == 0x8000u &&
             msg_index(msix_tables[sent[i].msix - 1][0]) < 0)) {
            return 1;
        }
    }
    for (i = 0; i < nrtes; i++) {
        uint16_t sid;

        if (dmar_ioapic_source(&vtd_dmar, rtes[i].id, &ui, &sid) == 0 && M(ui) == m &&
            (rtes[i].high & 0x10000u) == 0) {
            return 1;
        }
    }
    return 0;
}

/* ---- DMAR tables ---- */

static uint8_t tb[512];
static unsigned int tb_len, tb_at; /* the table's length, the structure's start */

/** Put the `n`-byte little-endian `v` at `at`. */
static void tb_put(unsigned int at, uint64_t v, int n)
{
    int i;

    for (i = 0; i < n; i++) {
        tb[at + (unsigned int)i] = (uint8_t)(v >> (8 * i));
    }
}

/** Start a table of host address width `haw` and `flags`. */
static void tb_begin(unsigned int haw, uint8_t flags)
{
    memset(tb, 0, sizeof(tb));
    memcpy(tb, "DMAR", 4);
    tb[8] = 1;
    memcpy(tb + 10, "LENOVO", 6);
    memcpy(tb + 16, "CB-01   ", 8);
    tb[36] = (uint8_t)(haw - 1u);
    tb[37] = flags;
    tb_len = 48;
}

/** Add a structure of `type` and `len` bytes. */
static void tb_struct(unsigned int type, unsigned int len)
{
    tb_at = tb_len;
    tb_put(tb_at, type, 2);
    tb_put(tb_at + 2, len, 2);
    tb_len += len;
}

/** Add a unit at `base` with `flags`. */
static void tb_drhd(uint8_t flags, uint64_t base)
{
    tb_struct(0, 16);
    tb[tb_at + 4] = flags;
    tb_put(tb_at + 8, base, 8);
}

/** Add a reserved region [base, end]. */
static void tb_rmrr(uint64_t base, uint64_t end)
{
    tb_struct(1, 24);
    tb_put(tb_at + 8, base, 8);
    tb_put(tb_at + 16, end, 8);
}

/** Add a scope of `type` from `bus` through `hops` (device, function)
 * pairs to the structure, and return where it starts. */
static unsigned int tb_scope(uint8_t type, uint8_t enum_id, uint8_t bus, int hops,
                             const uint8_t *path)
{
    unsigned int at = tb_len, len = 6u + 2u * (unsigned int)hops;

    tb[at] = type;
    tb[at + 1] = (uint8_t)len;
    tb[at + 4] = enum_id;
    tb[at + 5] = bus;
    memcpy(tb + at + 6, path, 2u * (unsigned int)hops);
    tb_len += len;
    tb_put(tb_at + 2, (tb[tb_at + 2] | (tb[tb_at + 3] << 8)) + len, 2);
    return at;
}

/** Set the length and the checksum. */
static void tb_end(void)
{
    unsigned int i;
    uint8_t sum = 0;

    tb_put(4, tb_len, 4);
    tb[9] = 0;
    for (i = 0; i < tb_len; i++) {
        sum = (uint8_t)(sum + tb[i]);
    }
    tb[9] = (uint8_t)(0u - sum);
    dmar_table = tb;
    dmar_len = tb_len;
}

/** The Lenovo's table, as the file's head describes. */
static void tb_lenovo(void)
{
    static const uint8_t gpu[] = {2, 0}, ioapic[] = {0x1F, 0}, hpet[] = {0x0F, 0};
    static const uint8_t ehci[] = {0x1D, 0}, xhci[] = {0x14, 0};

    tb_begin(39, DMAR_INTR_REMAP);
    tb_drhd(0, 0xfed90000ull);
    tb_scope(1, 0, 0, 1, gpu);
    tb_drhd(DMAR_INCLUDE_ALL, 0xfed91000ull);
    tb_scope(3, 2, 0xF0, 1, ioapic);
    tb_scope(4, 0, 0xF0, 1, hpet);
    tb_rmrr(0xaae11000ull, 0xaae30fffull);
    tb_scope(1, 0, 0, 1, ehci);
    tb_scope(1, 0, 0, 1, xhci);
    tb_rmrr(0xab800000ull, 0xaf9fffffull);
    tb_scope(1, 0, 0, 1, gpu);
    tb_end();
}

/* ---- the rows ---- */

static int fails, checks;
static const char *row;

/** Count a check, and print it when `ok` is 0. */
static void check(int ok, const char *what)
{
    checks++;
    if (!ok) {
        fails++;
        printf("vtd_hosttest: FAIL: %s: %s\n", row, what);
    }
}

/** Start row `name` with the machine empty. */
static void row_begin(const char *name)
{
    row = name;
    loglen = 0;
    logbuf[0] = 0;
    nmodels = 0;
    nsent = 0;
    nrtes = 0;
    nioapic_ids = 0;
    ram_top = 0x100000000ull;
    opt_intel_iommu = opt_intremap = NULL;
    opt_noapic = 0;
    fault_vec = 0x31;
    page_fail_after = -1;
    flush_calls = stray_flushes = 0;
    boot_apic = boot_x2apic = 0;
    now_ms += 60000u;
    x2_possible = x2_on = x2_moves = 0;
    lines_masked = x2_moved_masked = x2_moved_cfi = 0;
    memset(ecam_mem, 0xFF, (unsigned long)ECAM_BUSES << 20);
    memset(msix_tables, 0, sizeof(msix_tables));
    pci_ecam_set((uintptr_t)ecam_mem, 0, ECAM_BUSES - 1);
    if (pci_list != pci_first) {
        kfree(pci_list);
    }
    pci_list = pci_first;
    pci_list_cap = PCI_FIRST_MAX;
    pci_list_len = -1;
}

/** End the row: messages taken back, the units' tables freed, and every
 * page given back, with nothing the specification forbids done. */
static void row_end(void)
{
    int i, b, failed = fails;

    for (i = 0; i < nsent; i++) {
        pci_device_t d = t_dev(sent[i].bus, sent[i].devfn);

        if (sent[i].msix) {
            pci_msix_disable(&d);
        } else {
            pci_msi_disable(&d);
        }
    }
    intremap_stop();
    for (i = 0; i < vtd_nunits; i++) {
        struct vtd_unit *u = &vtd_units[i];

        for (b = 0; b < 256; b++) {
            if (u->ctx[b] != NULL) {
                vtd_hw_page_free(u->ctx[b]);
            }
        }
        if (u->root != NULL) {
            vtd_hw_page_free(u->root);
        }
        if (u->iq != NULL) {
            vtd_hw_page_free(u->iq);
            vtd_hw_page_free((void *)(uintptr_t)u->iq_status);
        }
        iommu_domain_free(u->identity);
    }
    for (i = 0; i < nmodels; i++) {
        check(models[i].violations == 0, "a command the specification forbids");
        check(models[i].stray == 0, "a unit read memory no table is in");
    }
    check(stray_flushes == 0, "a flush of memory no table is in");
    memset(vtd_units, 0, sizeof(vtd_units));
    memset(&vtd_dmar, 0, sizeof(vtd_dmar));
    vtd_nunits = 0;
    vtd_up = 0;
    vtd_dma_off = 0;
    check(mpage_count == 0, "a page was not given back");
    if (fails != failed) {
        printf("vtd_hosttest: the log of %s:\n%s", row, logbuf);
    }
}

/** QEMU's machine: its table, its unit, which leaves CFIS clear, its
 * I/O APIC with two lines routed, and its devices. */
static struct model *setup_qemu(uint64_t cap, uint64_t ecap)
{
    struct model *m = m_add(0xfed90000ull, cap, ecap);

    m->cfi_quiet = 1;
    pci_qemu();
    dmar_table = qemu_q35_dmar;
    dmar_len = sizeof(qemu_q35_dmar);
    ioapic_ids[nioapic_ids++] = 0;
    t_rte(0, 33);
    t_rte(0, 41 | (1u << 15));
    return m;
}

/** The Lenovo's machine. */
static void setup_lenovo(void)
{
    m_add(0xfed90000ull, CAP_LENOVO0, ECAP_LENOVO0);
    m_add(0xfed91000ull, CAP_LENOVO1, ECAP_LENOVO1);
    pci_lenovo();
    tb_lenovo();
    ioapic_ids[nioapic_ids++] = 2;
    t_rte(2, 33);
}

/** QEMU's table, as captured. */
static void row_dmar_qemu(void)
{
    struct dmar_info d;
    uint16_t sid = 0;
    int unit = -1;

    row_begin("dmar_qemu");
    check(dmar_parse(qemu_q35_dmar, sizeof(qemu_q35_dmar), &d) == 0, "parsed");
    check(d.haw == 48 && d.flags == 1 && d.nunits == 1 && d.nrmrrs == 0, "header");
    check(d.units[0].base == 0xfed90000ull && d.units[0].flags == 0 && d.units[0].nscopes == 8,
          "the unit and its eight scopes");
    check(d.units[0].scopes[0].type == 3 && d.units[0].scopes[0].bus == 0xFF, "the ioapic's");
    check(dmar_unit_for(&d, 0, DF(3, 0)) == 0 && dmar_unit_for(&d, 0, DF(0x1F, 2)) == 0,
          "listed devices are the unit's");
    check(dmar_unit_for(&d, 0, DF(4, 0)) == -1, "a device not listed is none's");
    check(dmar_ioapic_source(&d, 0, &unit, &sid) == 0 && unit == 0 && sid == 0xFF00,
          "the ioapic's requester id");
    check(dmar_ioapic_source(&d, 1, &unit, &sid) == -1, "no other ioapic");
    row_end();
}

/** The Lenovo's table, rebuilt. */
static void row_dmar_lenovo(void)
{
    struct dmar_info d;
    uint16_t sid = 0;
    int unit = -1;

    row_begin("dmar_lenovo");
    pci_lenovo();
    tb_lenovo();
    check(tb_len == 0xB0, "176 bytes, as Linux's log gives");
    check(dmar_parse(tb, tb_len, &d) == 0, "parsed");
    check(d.haw == 39 && d.nunits == 2 && d.nrmrrs == 2, "header");
    check(d.units[1].flags == DMAR_INCLUDE_ALL && d.units[1].base == 0xfed91000ull, "dmar1");
    check(dmar_unit_for(&d, 0, DF(2, 0)) == 0, "the GPU is dmar0's");
    check(dmar_unit_for(&d, 0, DF(0x14, 0)) == 1 && dmar_unit_for(&d, 1, DF(0, 0)) == 1 &&
              dmar_unit_for(&d, 2, DF(0, 0)) == 1,
          "the rest are dmar1's");
    check(dmar_rmrr_names(&d.rmrrs[0], 0, DF(0x14, 0)) &&
              dmar_rmrr_names(&d.rmrrs[0], 0, DF(0x1D, 0)) &&
              !dmar_rmrr_names(&d.rmrrs[0], 0, DF(2, 0)),
          "the USB region names the USB controllers alone");
    check(dmar_rmrr_names(&d.rmrrs[1], 0, DF(2, 0)) && d.rmrrs[1].end == 0xaf9fffffull,
          "the graphics region");
    check(dmar_ioapic_source(&d, 2, &unit, &sid) == 0 && unit == 1 && sid == 0xF0F8,
          "ioapic 2 under dmar1");
    row_end();
}

/** Refuse `t`, of `len` bytes. */
static int refused(const uint8_t *t, uint32_t len)
{
    struct dmar_info d;

    return dmar_parse(t, len, &d) == -1;
}

/** Malformed tables, and scopes whose paths cross bridges. */
static void row_dmar_malformed(void)
{
    static const uint8_t one[] = {3, 0}, far[] = {0x1C, 0, 0, 0}, bridge[] = {0x1C, 0};
    uint8_t copy[128];
    struct dmar_info d;
    unsigned int at;

    row_begin("dmar_malformed");
    memcpy(copy, qemu_q35_dmar, sizeof(copy));
    copy[0] = 'X';
    check(refused(copy, sizeof(copy)), "a bad signature");
    memcpy(copy, qemu_q35_dmar, sizeof(copy));
    copy[40] ^= 1;
    check(refused(copy, sizeof(copy)), "a bad checksum");
    check(refused(qemu_q35_dmar, sizeof(qemu_q35_dmar) - 8), "a length the header does not say");
    check(refused(NULL, 0), "no table");
    tb_begin(39, 0);
    tb_drhd(0, 0xfed90000ull);
    at = tb_scope(1, 0, 0, 1, one);
    tb[at + 1] = 7;
    tb_end();
    check(refused(tb, tb_len), "a scope of an odd length");
    tb_begin(39, 0);
    tb_drhd(0, 0xfed90000ull);
    tb_put(tb_at + 2, 200, 2);
    tb_len = 64;
    tb_end();
    check(refused(tb, tb_len), "a structure past the end");
    tb_begin(39, 0);
    tb_rmrr(0x2000, 0x1000);
    tb_end();
    check(refused(tb, tb_len), "a region that ends before it starts");

    tb_begin(39, 0);
    tb_drhd(0, 0xfed90000ull);
    tb_scope(1, 0, 0, 2, far);
    tb_scope(2, 0, 0, 1, bridge);
    tb_end();
    check(dmar_parse(tb, tb_len, &d) == 0 && d.units[0].scopes[0].bus == 0xFF,
          "a path through an absent bridge names nothing");
    cf_bridge(0, DF(0x1C, 0), 1, 3, 4);
    cf_dev(1, DF(0, 0), 0x028000, 0);
    check(dmar_parse(tb, tb_len, &d) == 0 && d.units[0].scopes[0].bus == 1 &&
              d.units[0].scopes[0].devfn == 0,
          "a path through a bridge ends on its secondary bus");
    check(d.units[0].scopes[1].sec == 1 && d.units[0].scopes[1].sub == 3 &&
              dmar_unit_for(&d, 3, DF(5, 0)) == 0 && dmar_unit_for(&d, 4, 0) == -1,
          "a bridge's scope covers its buses");
    row_end();
}

/** The message (address, data) of MSI of (bus, devfn). */
static void msi_of(uint8_t bus, uint8_t devfn, uint32_t *address, uint32_t *data)
{
    *address = cf32(bus, devfn, 0x54);
    *data = cf16(bus, devfn, 0x5C);
}

/** The interrupts of QEMU's machine before and after remapping, and new
 * and withdrawn messages while it is on. */
static void row_ir_qemu(void)
{
    struct model *m;
    uint32_t a, d, ahci_a, ahci_d, nvme_a;
    char want[200];
    pci_device_t e1000 = t_dev(0, DF(2, 0));

    row_begin("ir_qemu");
    m = setup_qemu(CAP_QEMU, ECAP_QEMU);
    check(t_msi(0, DF(0x1F, 2), 49) == 0 && t_msix(0, DF(3, 0), 0, 50) == 0, "messages");
    msi_of(0, DF(0x1F, 2), &a, &d);
    check(a == 0xFEE00000u && d == 49, "AHCI's message before remapping");
    check(vtd_start() == 1, "one unit");
    check(logged("vtd: dmar0: at 0xfed90000, version 1.0, cap 0x80d2008c222f0606, ecap 0xf00f4a"),
          "the unit's line");
    check(logged("vtd: interrupt remapping: on, xapic format, compatibility format refused") &&
              logged("vtd: ready: 1 units, interrupt remapping on, translation on for assigned "
                     "devices only"),
          "the ready lines");
    check(!logged("no answer"), "CFI's status is not waited for");
    check((m->gsts & (VTD_GCMD_IRE | VTD_GCMD_QIE)) == (VTD_GCMD_IRE | VTD_GCMD_QIE) &&
              (m->gsts & VTD_GCMD_TE) == 0 && (m->cmd & VTD_GCMD_CFI) == 0,
          "remapping and the queue on, translation off, compatibility format refused");
    check(iommu_irq_remapped(), "remapped");

    msi_of(0, DF(0x1F, 2), &ahci_a, &ahci_d);
    check((ahci_a & 0x10u) != 0 && m_msi(m, 0x00FA, ahci_a, ahci_d) == 49, "AHCI's MSI");
    nvme_a = msix_tables[0][0];
    check((nvme_a & 0x10u) != 0 && m_msi(m, 0x0018, nvme_a, msix_tables[0][2]) == 50,
          "NVMe's MSI-X");
    check((rtes[0].high & 0x10000u) != 0 && m_rte(m, 0xFF00, rtes[0].low, rtes[0].high) == 33,
          "the ioapic's edge line");
    check(m_rte(m, 0xFF00, rtes[1].low, rtes[1].high) == 41 && m->last_level == 1,
          "the ioapic's level line");

    check(m_msi(m, 0x00FA, nvme_a, 0) == -1, "a message from the wrong requester is refused");
    vtd_fault_all();
    snprintf(want, sizeof(want),
             "vtd: fault: dmar0: interrupt from 0:1f.2, index 0x%x: reason 0x26, Blocked an "
             "interrupt request due to source-id verification failure",
             (nvme_a >> 5) & 0x7FFFu);
    check(iommu_irq_faults() == 1 && logged(want), "and reported");
    check(m_msi(m, 0x00FA, 0xFEE00000u, 49) == -1, "a compatibility-format message is refused");
    vtd_fault_all();
    check(iommu_irq_faults() == 2 &&
              logged("reason 0x25, Blocked a compatibility format interrupt request"),
          "and reported");

    check(t_msi(0, DF(2, 0), 51) == 0 && logged("pci: msi: 0:2.0 on vector 51, remapped"),
          "a message enabled while remapping is on");
    msi_of(0, DF(2, 0), &a, &d);
    check(m_msi(m, 0x0010, a, d) == 51, "is remapped");
    check(t_msi(0, DF(0x1F, 2), 52) == 0, "a message enabled again");
    msi_of(0, DF(0x1F, 2), &a, &d);
    check(a != ahci_a && m_msi(m, 0x00FA, a, d) == 52, "takes a new entry");
    check(m_msi(m, 0x00FA, ahci_a, ahci_d) == -1, "and the old one is gone");
    vtd_fault_all();
    check(logged("reason 0x22, Present field in the IRTE entry is clear"), "as reported");
    pci_msi_disable(&e1000);
    msi_of(0, DF(2, 0), &a, &d);
    check((cf16(0, DF(2, 0), 0x52) & 1u) == 0 && a == 0xFEE00000u && d == 51,
          "a message disabled is left in the compatibility format");
    check(t_msi(0, DF(2, 0), 53) == 0, "enabled again");
    msi_of(0, DF(2, 0), &a, &d);
    check(msg_index(a) == msg_index(ahci_a) && m_msi(m, 0x0010, a, d) == 53,
          "an entry the unit held as not present is seen when it is taken again");

    intremap_stop();
    check((m->gsts & VTD_GCMD_IRE) == 0 && logged("vtd: interrupt remapping: off"), "off");
    msi_of(0, DF(0x1F, 2), &a, &d);
    check(a == 0xFEE00000u && d == 52 && m_msi(m, 0x00FA, a, d) == 52, "AHCI's message back");
    check(msix_tables[0][0] == 0xFEE00000u && msix_tables[0][2] == 50, "NVMe's back");
    check(rtes[0].high == 0 && m_rte(m, 0xFF00, rtes[0].low, rtes[0].high) == 33, "the line's");
    row_end();
}

/** The Lenovo's two units: interrupts remapped through tables only flushed
 * writes reach. */
static void row_ir_lenovo(void)
{
    uint32_t a, d;

    row_begin("ir_lenovo");
    setup_lenovo();
    check(t_msi(0, DF(2, 0), 49) == 0 && t_msi(0, DF(0x14, 0), 48) == 0 &&
              t_msi(1, DF(0, 0), 50) == 0 && t_msix(2, DF(0, 0), 1, 51) == 0,
          "messages");
    check(vtd_start() == 2, "two units");
    check(logged("vtd: dmar0: at 0xfed90000, version 1.0, cap 0xc0000020660462, ecap 0xf0101a") &&
              logged("vtd: dmar1: at 0xfed91000, version 1.0, cap 0xd2008020660462, ecap 0xf010da"),
          "the units' lines, as Linux's");
    check(intremap_active() && (M(0)->gsts & VTD_GCMD_IRE) && (M(1)->gsts & VTD_GCMD_IRE), "on");
    check(flush_calls > 0, "the tables were written back");
    msi_of(0, DF(2, 0), &a, &d);
    check(m_msi(M(0), 0x0010, a, d) == 49, "the GPU's message, through dmar0");
    msi_of(0, DF(0x14, 0), &a, &d);
    check(m_msi(M(1), 0x00A0, a, d) == 48, "xHCI's, through dmar1");
    msi_of(1, DF(0, 0), &a, &d);
    check(m_msi(M(1), 0x0100, a, d) == 50, "the AR9565's, behind a root port");
    check(m_msi(M(1), 0x0200, msix_tables[1][0], msix_tables[1][2]) == 51, "the RTL8168's");
    check(m_rte(M(1), 0xF0F8, rtes[0].low, rtes[0].high) == 33, "the ioapic's line");
    check(m_rte(M(1), 0x00F8, rtes[0].low, rtes[0].high) == -1, "only from the ioapic");
    check(logged(": ioapic 2, vector 33 to apic 0, ") && logged(", sid 0xf0f8 svt 1") &&
              logged(": msi of 2:0.0, vector 51 to apic 0, edge, sid 0x0200 svt 1") &&
              log_count("vtd: irte: dmar0 ") == 1 && log_count("vtd: irte: dmar1 ") == 4,
          "each entry logged");
    row_end();
}

/**
 * The Lenovo's units with a processor that offers x2APIC mode: the entries
 * in the x2APIC format, as Linux enables them there, the local APIC moved
 * once before the first entry, and every interrupt delivered to the boot
 * processor's id from bits 63:32. A table that opts out, or a unit without
 * the format, keeps the xAPIC format and the APIC where it is; a start that
 * fails after the move moves it back.
 */
static void row_ir_x2apic(void)
{
    uint32_t a, d;
    long held;

    row_begin("ir_x2apic");
    setup_lenovo();
    boot_apic = boot_x2apic = 2;
    x2_possible = 1;
    check(t_msi(0, DF(2, 0), 49) == 0 && t_msix(2, DF(0, 0), 1, 51) == 0, "messages");
    check(vtd_start() == 2 && intremap_active(), "on");
    check(x2_on && x2_moves == 1, "the APIC moved to x2APIC mode once");
    check(x2_moved_masked && !x2_moved_cfi && !lines_masked,
          "with the lines masked, after the compatibility format was refused, and unmasked after");
    check((M(0)->irta & (1u << 11)) != 0 && (M(1)->irta & (1u << 11)) != 0,
          "both tables in the x2APIC format");
    check(logged("vtd: interrupt remapping: on, x2apic format, compatibility format refused"),
          "logged");
    msi_of(0, DF(2, 0), &a, &d);
    check(m_msi(M(0), 0x0010, a, d) == 49 && M(0)->last_dest == 2, "the GPU's message to apic 2");
    check(m_msi(M(1), 0x0200, msix_tables[1][0], msix_tables[1][2]) == 51 && M(1)->last_dest == 2,
          "the RTL8168's");
    check(m_rte(M(1), 0xF0F8, rtes[0].low, rtes[0].high) == 33 && M(1)->last_dest == 0,
          "the ioapic's line to the id its entry names");
    row_end();

    row_begin("ir_x2apic_optout");
    setup_lenovo();
    tb[37] |= DMAR_X2APIC_OUT;
    tb[9] = (uint8_t)(tb[9] - DMAR_X2APIC_OUT); /* the checksum still sums to zero */
    boot_apic = boot_x2apic = 2;
    x2_possible = 1;
    check(t_msi(0, DF(2, 0), 49) == 0, "a message");
    check(vtd_start() == 2 && intremap_active(), "on");
    check(!x2_on && x2_moves == 0 && (M(0)->irta & (1u << 11)) == 0, "in the xAPIC format");
    check(logged("xapic format: the firmware opts out of x2apic"), "logged");
    msi_of(0, DF(2, 0), &a, &d);
    check(m_msi(M(0), 0x0010, a, d) == 49 && M(0)->last_dest == 2, "delivered to apic 2");
    row_end();

    row_begin("ir_x2apic_no_eim");
    setup_qemu(CAP_QEMU, ECAP_QEMU);
    x2_possible = 1;
    check(vtd_start() == 1 && intremap_active(), "on");
    check(!x2_on && (M(0)->irta & (1u << 11)) == 0, "QEMU's unit has no x2APIC format");
    check(logged("xapic format: dmar0 has no x2apic format"), "logged");
    row_end();

    row_begin("ir_x2apic_count");
    setup_lenovo();
    x2_possible = 1;
    check(vtd_start() == 2 && intremap_active(), "on");
    held = mpage_count; /* the last page taken is dmar1's remapping table */
    row_end();

    row_begin("ir_x2apic_back");
    setup_lenovo();
    x2_possible = 1;
    page_fail_after = held - 1;
    (void)vtd_start();
    check(!intremap_active() && x2_moves == 0 && !x2_on,
          "a start whose second table fails leaves the APIC in xAPIC mode, never moved");
    check(logged("vtd: ready: 2 units, interrupt remapping off"), "and runs unremapped");
    row_end();

    row_begin("ir_x2apic_last_step");
    setup_lenovo();
    x2_possible = 1;
    rtes[0].low |= 1u << 8; /* lowest priority, which ioapic_map refuses */
    check(t_msi(0, DF(2, 0), 49) == 0, "a message");
    (void)vtd_start();
    msi_of(0, DF(2, 0), &a, &d);
    check(!intremap_active() && x2_moves == 0 && !x2_on && !lines_masked,
          "a start that fails at its last fallible step, the lines masked, leaves the APIC");
    check(a == 0xFEE00000u && d == 49, "and the message as it was");
    row_end();
}

/**
 * An identity of 255 or more, which only remapping in the x2APIC format
 * reaches. The APIC starts in xAPIC mode, as `lapic_init` leaves it on bare
 * metal, so its id there is the low eight bits; the compatibility-format
 * message and redirection entry carry those, and the entries take the
 * whole x2APIC id, which the processor answers to after the move. Without
 * remapping the message's address holds the eight bits and nothing above
 * bit 19.
 */
static void row_ir_x2apic_wide(void)
{
    uint32_t a, d;

    row_begin("ir_x2apic_wide");
    setup_lenovo();
    boot_apic = 0x03;
    boot_x2apic = 0x103;
    x2_possible = 1;
    rtes[0].dest = 0x03;
    rtes[0].high = 0x03u << 24;
    check(t_msi(0, DF(2, 0), 49) == 0 && t_msix(2, DF(0, 0), 1, 51) == 0, "messages");
    msi_of(0, DF(2, 0), &a, &d);
    check(a == 0xFEE03000u, "the unremapped message names the low eight bits alone");
    check(vtd_start() == 2 && intremap_active() && x2_on, "on, in the x2APIC format");
    msi_of(0, DF(2, 0), &a, &d);
    check(m_msi(M(0), 0x0010, a, d) == 49 && M(0)->last_dest == 0x103,
          "the GPU's message to apic 0x103");
    check(m_msi(M(1), 0x0200, msix_tables[1][0], msix_tables[1][2]) == 51 &&
              M(1)->last_dest == 0x103,
          "the RTL8168's");
    check(m_rte(M(1), 0xF0F8, rtes[0].low, rtes[0].high) == 33 && M(1)->last_dest == 0x103,
          "the ioapic's line");
    check(M(0)->regs[VTD_FEADDR / 4] == 0xFEE03000u && M(0)->regs[VTD_FEUADDR / 4] == 0x100u &&
              M(1)->regs[VTD_FEADDR / 4] == 0xFEE03000u && M(1)->regs[VTD_FEUADDR / 4] == 0x100u &&
              M(0)->regs[VTD_FEDATA / 4] == 0x31u && M(0)->regs[VTD_FECTL / 4] == 0,
          "the fault events aimed again after the move, the id's bits 8 to 31 in FEUADDR");
    row_end();

    row_begin("ir_x2apic_wide_off");
    setup_lenovo();
    boot_apic = boot_x2apic = 0x103; /* a firmware's x2APIC mode, kept */
    opt_intremap = "off";
    check(t_msi(0, DF(2, 0), 49) == 0, "a message");
    check(vtd_start() == 2 && !intremap_active(), "remapping off");
    msi_of(0, DF(2, 0), &a, &d);
    check(a == 0xFEE03000u && d == 49, "the message keeps the low eight bits");
    check(M(0)->regs[VTD_FEADDR / 4] == 0xFEE03000u && M(0)->regs[VTD_FEUADDR / 4] == 0x100u,
          "and the unit's own fault event the rest in FEUADDR, as __irq_msi_compose_msg");
    row_end();
}

/** The requester id a message is checked against, behind each kind of
 * bridge. */
static void row_msi_source(void)
{
    unsigned int svt = 0;
    uint16_t sid = 0;

    row_begin("msi_source");
    cf_bridge(0, DF(0x1C, 0), 1, 1, 4);
    cf_dev(1, DF(0, 0), 0x028000, 0);
    cf_bridge(0, DF(0x1E, 0), 2, 2, -1);
    cf_dev(2, DF(3, 0), 0x020000, 0);
    cf_bridge(0, DF(0x1C, 2), 3, 5, 4);
    cf_bridge(3, DF(0, 0), 4, 4, 7);
    cf_dev(4, DF(5, 0), 0x040100, 0);
    cf_bridge(3, DF(1, 0), 5, 5, 8);
    cf_dev(5, DF(0, 0), 0x010802, 0);
    msi_source(0, DF(0x1F, 2), &svt, &sid);
    check(svt == 1 && sid == 0x00FA, "on the root bus: itself");
    msi_source(1, DF(0, 0), &svt, &sid);
    check(svt == 1 && sid == 0x0100, "behind a root port: itself");
    msi_source(2, DF(3, 0), &svt, &sid);
    check(svt == 1 && sid == 0x00F0, "behind a PCI bridge: the bridge");
    msi_source(4, DF(5, 0), &svt, &sid);
    check(svt == 2 && sid == 0x0404, "behind a PCIe-to-PCI bridge: its secondary bus");
    msi_source(5, DF(0, 0), &svt, &sid);
    check(svt == 1 && sid == 0x0308, "behind a PCI-to-PCIe bridge: the bridge");
    row_end();
}

/** The leaf of `d` for `iova`, or 0. */
static uint64_t leaf_of(struct iommu_domain *d, uint64_t iova)
{
    int found = 0;
    uint64_t *slot = dom_slot(d, iova, 1, 0, &found);

    return slot != NULL ? *slot : 0;
}

/** The Lenovo's reserved regions, as Linux reports them. */
static void row_regions_lenovo(void)
{
    struct iommu_region r[4];

    row_begin("regions_lenovo");
    setup_lenovo();
    check(vtd_start() == 2, "up");
    check(iommu_reserved_regions(0, DF(2, 0), r, 4) == 2 && r[0].start == 0xab800000ull &&
              r[0].end == 0xaf9fffffull && r[0].type == IOMMU_RESV_DIRECT_RELAXABLE &&
              r[1].start == 0xFEE00000ull && r[1].type == IOMMU_RESV_MSI,
          "the GPU's region is relaxable");
    check(iommu_reserved_regions(0, DF(0x14, 0), r, 4) == 2 && r[0].start == 0xaae11000ull &&
              r[0].type == IOMMU_RESV_DIRECT_RELAXABLE,
          "xHCI's, as Linux's device_rmrr_is_relaxable has USB");
    check(iommu_reserved_regions(0, DF(0x1D, 0), r, 4) == 2 && r[0].start == 0xaae11000ull,
          "EHCI's");
    check(iommu_reserved_regions(0, DF(0x1F, 0), r, 4) == 2 && r[0].start == 0 &&
              r[0].end == 0xFFFFFFull && r[0].type == IOMMU_RESV_DIRECT_RELAXABLE,
          "the ISA bridge's floppy range");
    check(iommu_reserved_regions(0, DF(0x1F, 2), r, 4) == 1 && r[0].type == IOMMU_RESV_MSI,
          "SATA's: the MSI window alone");
    row_end();
}

/** The GPU given to a domain on the Lenovo: dmar0 translates through an
 * identity domain of 4 KiB pages, flushed, without the snoop bit. */
static void row_assign_gpu(void)
{
    struct iommu_domain *d;
    struct vtd_unit *u0 = &vtd_units[0];
    uint64_t phys = 0;
    unsigned long tlb;

    row_begin("assign_gpu");
    setup_lenovo();
    check(vtd_start() == 2 && iommu_present(), "up");
    d = iommu_domain_alloc();
    check(d != NULL && d->levels == 4 && iommu_domain_limit(d) == 1ull << 48, "a domain");
    check(iommu_map(d, 0x100000, 0x7654000, 0x10000, IOMMU_READ | IOMMU_WRITE) == 0, "mapped");
    check(iommu_attach(d, 0, DF(2, 0)) == 0, "the GPU attached");
    check(logged("vtd: dmar0: translation on") && logged("vtd: attach: 0:2.0 to domain 2 of dmar0"),
          "logged");
    check((M(0)->gsts & VTD_GCMD_TE) != 0 && (M(1)->gsts & VTD_GCMD_TE) == 0,
          "dmar0 translates, dmar1 does not");
    check(iommu_domain_limit(d) == 1ull << 39, "the domain is held to dmar0's 39 bits");
    check(m_dma(M(0), 0, DF(2, 0), 0x101234, 1, &phys) == 0 && phys == 0x7655234,
          "the GPU's DMA is translated");
    check((leaf_of(d, 0x100000) & VTD_PTE_SNP) == 0 && !d->snoop && d->flush,
          "no snoop bit, and flushed, on a unit without either");
    check(u0->identity != NULL && u0->identity->pages == 0x100000ul &&
              iommu_iova_to_phys(u0->identity, 0xab800000ull) == 0xab800000ull,
          "dmar0's identity domain: 4 GiB of 4 KiB pages");
    check(iommu_map(d, 0x40000000, 0x7654000, 0x1000, IOMMU_READ) == 0 &&
              m_dma(M(0), 0, DF(2, 0), 0x41000000, 0, &phys) == 6,
          "a table made while attached is written back whole");
    vtd_fault_all();
    check(m_dma(M(0), 0, DF(2, 0), 0x200000, 1, &phys) == 5, "past the mapping, a fault");
    vtd_fault_all();
    check(iommu_dma_faults() == 2 &&
              logged("vtd: fault: dmar0: dma write from 0:2.0 at 0x200000: reason 0x5, PTE Write "
                     "access is not set"),
          "reported");
    tlb = M(0)->inv_tlb;
    check(iommu_map(d, 0x300000, 0x9000000, 0x1000, IOMMU_READ) == 0 && M(0)->inv_tlb == tlb,
          "a mapping added needs no invalidation outside caching mode");
    check(m_dma(M(0), 0, DF(2, 0), 0x300000, 1, &phys) == 5 &&
              m_dma(M(0), 0, DF(2, 0), 0x300000, 0, &phys) == 0 && phys == 0x9000000,
          "a mapping for reading alone");
    vtd_fault_all();
    check(m_dma(M(0), 0, DF(2, 0), 0x100000, 0, &phys) == 0, "read before the unmap");
    check(iommu_unmap(d, 0x100000, 0x10000) == 0 && d->pages == 2, "unmapped");
    check(m_dma(M(0), 0, DF(2, 0), 0x100000, 0, &phys) == 6, "and the IOTLB forgot it");
    vtd_fault_all();
    iommu_detach(d, 0, DF(2, 0));
    check((M(0)->gsts & VTD_GCMD_TE) == 0 && logged("vtd: dmar0: translation off"), "detached");
    check(m_dma(M(0), 0, DF(2, 0), 0x5000, 1, &phys) == 0 && phys == 0x5000,
          "the GPU's DMA is physical again");
    iommu_domain_free(d);
    row_end();
}

/** A device of dmar1 given to a domain: the rest of its devices pass
 * through, the leaves snoop, and that domain cannot go to dmar0. */
static void row_assign_pt(void)
{
    struct iommu_domain *d;
    uint64_t phys = 0;

    row_begin("assign_pt");
    setup_lenovo();
    check(vtd_start() == 2, "up");
    d = iommu_domain_alloc();
    check(iommu_attach(d, 1, DF(0, 0)) == 0 && d->snoop, "the AR9565 attached, snooping");
    check(iommu_map(d, 0x40000000, 0x12000000, 0x2000, IOMMU_READ | IOMMU_WRITE) == 0, "mapped");
    check((leaf_of(d, 0x40000000) & VTD_PTE_SNP) != 0, "the leaf has the snoop bit");
    check(m_dma(M(1), 1, DF(0, 0), 0x40001010, 1, &phys) == 0 && phys == 0x12001010,
          "its DMA is translated");
    check(m_dma(M(1), 0, DF(0x1F, 2), 0x12345000, 1, &phys) == 0 && phys == 0x12345000,
          "SATA passes through");
    check(m_dma(M(1), 2, DF(0, 0), 0x87654000, 0, &phys) == 0 && phys == 0x87654000,
          "the RTL8168 passes through");
    check(vtd_units[1].identity == NULL, "no identity domain where pass-through exists");
    check(iommu_attach(d, 0, DF(2, 0)) == -EINVAL, "a snooping domain refused by dmar0");
    check(iommu_attach(d, 1, DF(0, 0)) == -EBUSY, "a device already given");
    check(iommu_attach(d, 0, DF(0x14, 0)) == 0, "xHCI, whose region is relaxable, attached");
    iommu_detach(d, 1, DF(0, 0));
    check((M(1)->gsts & VTD_GCMD_TE) != 0, "still translating for xHCI");
    check(m_dma(M(1), 1, DF(0, 0), 0x40001000, 0, &phys) == 0 && phys == 0x40001000,
          "the AR9565 passes through again");
    iommu_detach(d, 0, DF(0x14, 0));
    check((M(1)->gsts & VTD_GCMD_TE) == 0 && d->did[1] == 0, "off, the domain's id given back");
    iommu_domain_free(d);
    row_end();
}

/** A coherent unit without pass-through: no flush at all, an identity
 * domain of 2 MiB pages to the end of RAM and over each reserved region,
 * and a device whose region must stay mapped refused. */
static void row_identity_coherent(void)
{
    static const uint8_t ahci[] = {0x1F, 2}, vga[] = {1, 0};
    struct iommu_domain *d, *id;
    uint64_t phys = 0;

    row_begin("identity_coherent");
    m_add(0xfed90000ull, CAP_QEMU, (ECAP_QEMU | 1u) & ~0x40ull);
    pci_qemu();
    ram_top = 0x140000000ull;
    tb_begin(48, 0);
    tb_drhd(DMAR_INCLUDE_ALL, 0xfed90000ull);
    tb_rmrr(0x180000000ull, 0x1801fffffull);
    tb_scope(1, 0, 0, 1, ahci);
    tb_rmrr(0xab800000ull, 0xab8fffffull);
    tb_scope(1, 0, 0, 1, vga);
    tb_end();
    check(vtd_start() == 1 && logged("interrupt remapping: off: the firmware does not offer it"),
          "up, without remapping");
    d = iommu_domain_alloc();
    check(iommu_attach(d, 0, DF(0x1F, 2)) == -EPERM &&
              logged("vtd: attach: 0:1f.2 refused: its reserved region must stay mapped"),
          "AHCI, whose region is not relaxable, refused");
    check(iommu_attach(d, 0, DF(1, 0)) == 0, "VGA attached");
    id = vtd_units[0].identity;
    check(id != NULL && id->pages == 0x140000ul + 0x200ul, "identity to 5 GiB and the region");
    check((leaf_of(id, 0) & VTD_PTE_PS) != 0, "in 2 MiB leaves");
    check(m_dma(M(0), 0, DF(0x1F, 2), 0x13fff0040ull, 1, &phys) == 0 && phys == 0x13fff0040ull,
          "AHCI's DMA below the end of RAM");
    check(m_dma(M(0), 0, DF(0x1F, 2), 0x180100000ull, 0, &phys) == 0 && phys == 0x180100000ull,
          "and to its region");
    check(m_dma(M(0), 0, DF(0x1F, 2), 0x190000000ull, 0, &phys) == 6, "but nowhere else");
    vtd_fault_all();
    check(flush_calls == 0, "a coherent unit has nothing written back");
    iommu_detach(d, 0, DF(1, 0));
    iommu_domain_free(d);
    row_end();
}

/** QEMU with caching mode: an entry made present is invalidated, since the
 * unit may hold it as not present. */
static void row_caching_mode(void)
{
    struct iommu_domain *d;
    uint64_t phys = 0;

    row_begin("caching_mode");
    setup_qemu(CAP_QEMU | 0x80u, ECAP_QEMU);
    check(vtd_start() == 1, "up");
    d = iommu_domain_alloc();
    check(iommu_attach(d, 0, DF(3, 0)) == 0, "NVMe attached");
    check(m_dma(M(0), 0, DF(3, 0), 0x400000, 0, &phys) == 6, "nothing mapped yet");
    vtd_fault_all();
    check(iommu_map(d, 0x400000, 0x8000000, 0x1000, IOMMU_READ | IOMMU_WRITE) == 0, "mapped");
    check(m_dma(M(0), 0, DF(3, 0), 0x400000, 0, &phys) == 0 && phys == 0x8000000,
          "seen, the cached absence invalidated");
    check(m_dma(M(0), 0, DF(0x1F, 2), 0x400000, 0, &phys) == 0 && phys == 0x400000,
          "AHCI passes through");
    iommu_detach(d, 0, DF(3, 0));
    iommu_domain_free(d);
    row_end();
}

/** A unit of three levels walks a domain of four from its first entry. */
static void row_levels(void)
{
    static const uint8_t nvme[] = {3, 0};
    uint64_t cap3 = (CAP_QEMU & ~((0x1Full << 8) | (0x3Full << 16))) | (2ull << 8) | (38ull << 16);
    struct iommu_domain *d, *far;
    uint64_t phys = 0;

    row_begin("levels");
    m_add(0xfed90000ull, CAP_QEMU, ECAP_QEMU);
    m_add(0xfed91000ull, cap3, ECAP_QEMU);
    pci_qemu();
    tb_begin(48, 0);
    tb_drhd(0, 0xfed90000ull);
    tb_scope(1, 0, 0, 1, nvme);
    tb_drhd(DMAR_INCLUDE_ALL, 0xfed91000ull);
    tb_end();
    check(vtd_start() == 2 && vtd_units[1].levels == 3, "two units, one of three levels");
    d = iommu_domain_alloc();
    far = iommu_domain_alloc();
    check(d->levels == 4, "a domain of four");
    check(iommu_map(d, 0x200000, 0x3000000, 0x1000, IOMMU_READ) == 0, "mapped");
    check(iommu_attach(d, 0, DF(0x1F, 2)) == 0 && iommu_attach(d, 0, DF(3, 0)) == 0,
          "on both units");
    check(m_dma(M(1), 0, DF(0x1F, 2), 0x200010, 0, &phys) == 0 && phys == 0x3000010,
          "the three-level unit's walk");
    check(m_dma(M(0), 0, DF(3, 0), 0x200010, 0, &phys) == 0 && phys == 0x3000010,
          "the four-level unit's");
    check(iommu_map(d, 1ull << 40, 0x1000, 0x1000, IOMMU_READ) == -EINVAL,
          "nothing past the three-level unit's reach");
    check(iommu_map(far, 1ull << 40, 0x1000, 0x1000, IOMMU_READ) == 0 &&
              iommu_attach(far, 0, DF(1, 0)) == -EINVAL,
          "and no domain mapped past it goes there");
    iommu_detach(d, 0, DF(0x1F, 2));
    iommu_detach(d, 0, DF(3, 0));
    iommu_domain_free(d);
    iommu_domain_free(far);
    row_end();
}

/** The domain calls' refusals, and a page that could not be had. */
static void row_domain_api(void)
{
    struct iommu_domain *d;

    row_begin("domain_api");
    setup_qemu(CAP_QEMU, ECAP_QEMU);
    check(vtd_start() == 1, "up");
    d = iommu_domain_alloc();
    check(iommu_map(d, 0x1001, 0x1000, 0x1000, IOMMU_READ) == -EINVAL &&
              iommu_map(d, 0x1000, 0x1000, 0, IOMMU_READ) == -EINVAL &&
              iommu_map(d, 1ull << 48, 0x1000, 0x1000, IOMMU_READ) == -EINVAL,
          "misaligned, empty and out of reach");
    check(iommu_map(d, 0x1000, 0x5000, 0x1000, IOMMU_READ) == 0 && d->pages == 1, "a page");
    check(iommu_map(d, 0x1000, 0x6000, 0x1000, IOMMU_READ) == -EEXIST, "not twice");
    check(iommu_map(d, 0, 0x6000, 0x3000, IOMMU_READ) == -EEXIST && d->pages == 1 &&
              iommu_iova_to_phys(d, 0) == ~0ull,
          "an overlap leaves nothing of itself");
    check(iommu_iova_to_phys(d, 0x1234) == 0x5234, "the address");
    page_fail_after = 0;
    check(iommu_map(d, 1ull << 40, 0x1000, 0x1000, IOMMU_READ) == -ENOMEM && d->pages == 1,
          "no page for a table");
    page_fail_after = -1;
    check(iommu_unmap(d, 0x1000, 0x1000) == 0 && d->pages == 0 &&
              iommu_iova_to_phys(d, 0x1000) == ~0ull,
          "unmapped");
    iommu_domain_free(d);
    row_end();
}

/** A refused descriptor is reported and the queue goes on. */
static void row_iqe(void)
{
    struct model *m;

    row_begin("iqe");
    m = setup_qemu(CAP_QEMU, ECAP_QEMU);
    check(vtd_start() == 1, "up");
    m->refuse_next = 1;
    check(vtd_inv_iec(&vtd_units[0], 5) == -1 && vtd_units[0].errors == 1 && m->iqe == 1,
          "refused");
    check(logged("vtd: dmar0: invalidation refused: 0x500000014 0x0"), "logged");
    check((m_fsts(m) & VTD_FSTS_IQE) == 0, "IQE cleared");
    check(vtd_inv_iec(&vtd_units[0], 6) == 0, "the queue goes on");
    row_end();
}

/** The boot line's `intel_iommu=off`, `intremap=off`, and `noapic` or
 * `maxcpus=0`, which keep remapping off as Linux's enable_IR_x2apic does. */
static void row_options(void)
{
    struct iommu_domain *d;
    struct model *m;
    uint32_t a, data;

    row_begin("intel_iommu_off");
    setup_qemu(CAP_QEMU, ECAP_QEMU);
    opt_intel_iommu = "off";
    check(vtd_start() == 1 && !iommu_present() && iommu_domain_alloc() == NULL, "no domains");
    d = dom_new(4);
    check(iommu_attach(d, 0, DF(3, 0)) == -ENODEV, "nothing attached");
    iommu_domain_free(d);
    check(intremap_active() && logged("translation off by intel_iommu=off"), "remapping on");
    row_end();

    row_begin("intremap_off");
    m = setup_qemu(CAP_QEMU, ECAP_QEMU);
    opt_intel_iommu = "on,igfx_off";
    opt_intremap = "off";
    check(t_msi(0, DF(0x1F, 2), 49) == 0, "a message");
    check(vtd_start() == 1 && iommu_present() && !intremap_active() &&
              (m->gsts & VTD_GCMD_IRE) == 0,
          "translation offered, remapping off");
    check(logged("vtd: interrupt remapping: off by nointremap or intremap=off"), "logged");
    msi_of(0, DF(0x1F, 2), &a, &data);
    check(a == 0xFEE00000u && data == 49, "the message as it was");
    row_end();

    row_begin("noapic");
    m = setup_qemu(CAP_QEMU, ECAP_QEMU);
    opt_noapic = 1;
    x2_possible = 1;
    check(t_msi(0, DF(0x1F, 2), 49) == 0, "a message");
    check(vtd_start() == 1 && iommu_present() && !intremap_active() &&
              (m->gsts & VTD_GCMD_IRE) == 0 && x2_moves == 0,
          "translation offered, remapping off, the APIC not moved");
    check(logged("vtd: interrupt remapping: off by noapic or maxcpus=0"), "logged");
    msi_of(0, DF(0x1F, 2), &a, &data);
    check(a == 0xFEE00000u && data == 49, "the message as it was");
    row_end();
}

/** `intel_iommu=off intremap=off` on the Lenovo's units: each read and
 * logged, none written, and none up; translation the firmware left on
 * turned off with one command. */
static void row_read_only(void)
{
    uint32_t a, data;

    row_begin("read_only");
    setup_lenovo();
    opt_intel_iommu = "off";
    opt_intremap = "off";
    check(t_msi(0, DF(0x1F, 2), 50) == 0, "a message");
    check(vtd_start() == -1 && vtd_nunits == 0 && !iommu_present() && !intremap_active(),
          "nothing up");
    check(models[0].writes == 0 && models[1].writes == 0, "no register written");
    check(logged("vtd: dmar0: at 0xfed90000, version 1.0, cap 0xc0000020660462, ecap 0xf0101a") &&
              logged("vtd: dmar1: at 0xfed91000, version 1.0, cap 0xd2008020660462, ecap 0xf010da"),
          "both units read");
    check(logged("vtd: dmar0: status 0x0") && logged("vtd: dmar1: status 0x0"), "status read");
    check(logged("vtd: off by intel_iommu=off and nointremap or intremap=off: 2 units read"),
          "logged");
    msi_of(0, DF(0x1F, 2), &a, &data);
    check(a == 0xFEE00000u && data == 50, "the message as it was");
    row_end();

    row_begin("read_only_te");
    setup_lenovo();
    models[1].gsts |= VTD_GCMD_TE;
    models[1].cmd |= VTD_GCMD_TE;
    opt_intel_iommu = "off";
    opt_intremap = "off";
    check(vtd_start() == -1 && vtd_nunits == 0, "nothing up");
    check(models[0].writes == 0 && models[1].writes == 1 && (models[1].gsts & VTD_GCMD_TE) == 0,
          "translation turned off, nothing else written");
    check(logged("vtd: dmar1: translation was on: turned off"), "logged");
    row_end();
}

/** The units' fault events held across the late x2APIC leave: each masked
 * in FECTL, where an event that comes waits in FECTL.IP, and unmasked
 * again, which sends it; nothing is written while no vector was taken. */
static void row_fault_hold(void)
{
    unsigned long w0, w1;

    row_begin("fault_hold");
    setup_lenovo();
    opt_intremap = "off";
    check(vtd_start() == 2, "up");
    iommu_fault_hold(1);
    check(M(0)->regs[VTD_FECTL / 4] == VTD_FECTL_IM && M(1)->regs[VTD_FECTL / 4] == VTD_FECTL_IM,
          "held: each unit's fault event masked");
    check(!M(0)->fectl_posted && !M(1)->fectl_posted,
          "and the mask read back, so the posted write reached the unit before the APIC moves");
    check(M(0)->regs[VTD_FEDATA / 4] == 0x31u && M(1)->regs[VTD_FEDATA / 4] == 0x31u,
          "and still aimed at its vector");
    iommu_fault_hold(0);
    check(M(0)->regs[VTD_FECTL / 4] == 0 && M(1)->regs[VTD_FECTL / 4] == 0 && !M(0)->fectl_posted &&
              !M(1)->fectl_posted,
          "let go: each unmasked and read back, which sends an event held meanwhile");
    row_end();

    row_begin("fault_hold_none");
    setup_lenovo();
    opt_intremap = "off";
    fault_vec = -1;
    check(vtd_start() == 2 && logged("vtd: fault: no vector: faults go unreported"),
          "up with no vector");
    w0 = models[0].writes;
    w1 = models[1].writes;
    iommu_fault_hold(1);
    iommu_fault_hold(0);
    check(models[0].writes == w0 && models[1].writes == w1,
          "nothing written while no vector was taken");
    row_end();
}

/** Faults past the rate limit, 10 lines in 5 seconds for every unit, are
 * counted, and one lost is counted too; the next window says how many
 * lines were held back. */
static void row_fault_limit(void)
{
    struct iommu_domain *d;
    uint64_t phys;
    int i;

    row_begin("fault_limit");
    setup_qemu(CAP_QEMU, ECAP_QEMU);
    check(vtd_start() == 1, "up");
    d = iommu_domain_alloc();
    check(iommu_attach(d, 0, DF(3, 0)) == 0, "NVMe attached to an empty domain");
    (void)m_dma(M(0), 0, DF(3, 0), 0x1000, 0, &phys);
    (void)m_dma(M(0), 0, DF(3, 0), 0x2000, 0, &phys);
    vtd_fault_all();
    check(iommu_dma_faults() == 2 && logged("vtd: fault: dmar0: faults lost, every record full"),
          "a fault lost while the one record was full");
    for (i = 0; i < 40; i++) {
        (void)m_dma(M(0), 0, DF(3, 0), 0x10000ull * (unsigned int)(i + 1), 0, &phys);
        vtd_fault_all();
    }
    check(iommu_dma_faults() == 42, "every fault counted");
    check(log_count("vtd: fault: dmar0: dma read from 0:3.0") == 9,
          "10 lines logged in the window, the loss's among them, and 32 held back");
    now_ms += 5001u;
    (void)m_dma(M(0), 0, DF(3, 0), 0x800000, 0, &phys);
    vtd_fault_all();
    check(iommu_dma_faults() == 43 && logged("vtd: fault: 32 lines suppressed") &&
              log_count("vtd: fault: dmar0: dma read from 0:3.0") == 10,
          "the next window logs the count held back, then the fault");
    check(logged("vtd: fault: dmar0: dma read from 0:3.0 at 0x1000: reason 0x6, PTE Read access is "
                 "not set"),
          "in Linux's words");
    iommu_detach(d, 0, DF(3, 0));
    iommu_domain_free(d);
    row_end();
}

/** A device that keeps faulting refills the one record as fast as it is
 * read: a pass stops at its bound and the next tick reads the rest. */
static void row_fault_bound(void)
{
    struct iommu_domain *d;
    uint64_t phys;

    row_begin("fault_bound");
    setup_qemu(CAP_QEMU, ECAP_QEMU);
    check(vtd_start() == 1, "up");
    d = iommu_domain_alloc();
    check(iommu_attach(d, 0, DF(3, 0)) == 0, "NVMe attached to an empty domain");
    M(0)->refill = 40;
    (void)m_dma(M(0), 0, DF(3, 0), 0x1000, 0, &phys);
    vtd_fault_all();
    check(iommu_dma_faults() == 32 && vtd_units[0].fault_again == 1 &&
              (m_fsts(M(0)) & VTD_FSTS_PPF) != 0,
          "a pass reads 32 records and leaves the full one");
    iommu_tick();
    check(iommu_dma_faults() == 41 && vtd_units[0].fault_again == 0 &&
              (m_fsts(M(0)) & VTD_FSTS_PPF) == 0,
          "the tick reads the other 9");
    iommu_tick();
    check(iommu_dma_faults() == 41, "and the next tick none");
    check(log_count("vtd: fault: dmar0: dma read from 0:3.0") == 10, "10 logged");
    now_ms += 5001u;
    (void)m_dma(M(0), 0, DF(3, 0), 0x800000, 0, &phys);
    vtd_fault_all();
    check(iommu_dma_faults() == 42 && logged("vtd: fault: 31 lines suppressed") &&
              log_count("vtd: fault: dmar0: dma read from 0:3.0") == 11,
          "and a fault in the next window logged after the 31 held back");
    iommu_detach(d, 0, DF(3, 0));
    iommu_domain_free(d);
    row_end();
}

/** A device that keeps faulting loses a fault on every pass: each loss is
 * counted, and the lines of a record and of a loss share the rate limit,
 * so the first 5 passes are logged, and a pass in the next window again. */
static void row_fault_lost_limit(void)
{
    struct iommu_domain *d;
    uint64_t phys;
    int i;

    row_begin("fault_lost_limit");
    setup_qemu(CAP_QEMU, ECAP_QEMU);
    check(vtd_start() == 1, "up");
    d = iommu_domain_alloc();
    check(iommu_attach(d, 0, DF(3, 0)) == 0, "NVMe attached to an empty domain");
    for (i = 0; i < 40; i++) {
        (void)m_dma(M(0), 0, DF(3, 0), 0x10000ull * (unsigned int)(i + 1), 0, &phys);
        (void)m_dma(M(0), 0, DF(3, 0), 0x10000ull * (unsigned int)(i + 1) + 0x1000u, 0, &phys);
        vtd_fault_all();
    }
    check(vtd_units[0].overflows == 40 && iommu_dma_faults() == 80, "every loss counted");
    check(log_count("vtd: fault: dmar0: faults lost, every record full") == 5 &&
              logged("vtd: fault: dmar0: faults lost, every record full: 5 times, so 10 DMA "
                     "faults is a floor") &&
              !logged("lines suppressed"),
          "5 losses logged");
    now_ms += 5001u;
    (void)m_dma(M(0), 0, DF(3, 0), 0x800000, 0, &phys);
    (void)m_dma(M(0), 0, DF(3, 0), 0x801000, 0, &phys);
    vtd_fault_all();
    check(logged("vtd: fault: 70 lines suppressed") &&
              logged("vtd: fault: dmar0: faults lost, every record full: 41 times, so 82 DMA "
                     "faults is a floor"),
          "the next window's loss logged after the 70 lines held back");
    iommu_detach(d, 0, DF(3, 0));
    iommu_domain_free(d);
    row_end();
}

/** A full remapping table refuses a message, and the device keeps none. */
static void row_irt_full(void)
{
    uint8_t used[VTD_IRT_ENTRIES / 8];

    row_begin("irt_full");
    setup_qemu(CAP_QEMU, ECAP_QEMU);
    check(vtd_start() == 1 && intremap_active(), "up");
    memcpy(used, vtd_units[0].irte_used, sizeof(used));
    memset(vtd_units[0].irte_used, 0xFF, sizeof(used));
    check(t_msi(0, DF(2, 0), 51) == -1 && (cf16(0, DF(2, 0), 0x52) & 1u) == 0,
          "refused, and not enabled");
    check(logged("interrupt remapping: table full"), "logged");
    memcpy(vtd_units[0].irte_used, used, sizeof(used));
    row_end();
}

/** 0: no row maps a window of a BAR (pci_bar_map). */
uintptr_t mmio_map(uint64_t phys, size_t size, pte_t cache)
{
    (void)phys;
    (void)size;
    (void)cache;
    return 0;
}

/** Nothing: no row maps a window of a BAR. */
void mmio_unmap(uintptr_t va, size_t size)
{
    (void)va;
    (void)size;
}

/** Migrate real PCI messages through the real remapper, with its chip
 * model retaining interrupt-entry caches until invalidated. */
static void row_ir_affinity(void)
{
    uint32_t address, data, before_address, before_data;
    uint64_t requested, effective;
    unsigned int c;

    row_begin("ir_affinity");
    setup_lenovo();
    x2_possible = 1;
    check(t_msi(0, DF(2, 0), 49) == 0 && t_msix(2, DF(0, 0), 1, 51) == 0,
          "enable the GPU's MSI and the network card's MSI-X");
    check(vtd_start() == 2 && intremap_active() && x2_on, "start the real remapper");
    for (c = 0; c < 4; c++) {
        cpu_table[c].online = 1;
        model_apic[c] = c == 3 ? 0x1234 : c;
    }
    msi_of(0, DF(2, 0), &before_address, &before_data);
    check(m_msi(M(0), 0x0010, before_address, before_data) == 49 && M(0)->last_dest == 0,
          "prime the model's old destination cache");
    check(pci_irq_set_affinity(49, 8) == 0, "move the GPU's interrupt to processor 3");
    msi_of(0, DF(2, 0), &address, &data);
    check(address == before_address && data == before_data &&
              m_msi(M(0), 0x0010, address, data) == 49 && M(0)->last_dest == 0x1234,
          "the same in-flight message handle reaches the whole new x2APIC id");
    check(pci_irq_set_affinity(51, 4) == 0 &&
              m_msi(M(1), 0x0200, msix_tables[1][0], msix_tables[1][2]) == 51 &&
              M(1)->last_dest == 2,
          "MSI-X reaches its new processor through the same remapper");
    cpu_table[3].online = 0;
    pci_msi_rebalance();
    check(!pci_irq_affinity(49, &requested, &effective) && !(effective & 8) && requested == 8,
          "a destination going offline breaks effective placement and retains the request");
    pci_msi_shutdown(0);
    check(m_msi(M(0), 0x0010, address, data) == 49 && M(0)->last_dest == 0,
          "normal shutdown leaves the live remapping entry aimed at processor 0");
    memset(cpu_table, 0, sizeof(cpu_table));
    row_end();
}

int main(int argc, char **argv)
{
    ecam_mem = (unsigned char *)aligned_alloc(4096, (unsigned long)ECAM_BUSES << 20);
    if (argc == 2 && !__builtin_strcmp(argv[1], "--affinity-only")) {
        row_ir_affinity();
        free(ecam_mem);
        printf("vtd_hosttest: affinity: %s: %d checks\n", fails ? "fail" : "pass", checks);
        return fails != 0;
    }
    row_ir_affinity();
    row_dmar_qemu();
    row_dmar_lenovo();
    row_dmar_malformed();
    row_ir_qemu();
    row_ir_lenovo();
    row_ir_x2apic();
    row_ir_x2apic_wide();
    row_msi_source();
    row_regions_lenovo();
    row_assign_gpu();
    row_assign_pt();
    row_identity_coherent();
    row_caching_mode();
    row_levels();
    row_domain_api();
    row_iqe();
    row_options();
    row_read_only();
    row_fault_hold();
    row_fault_limit();
    row_fault_bound();
    row_fault_lost_limit();
    row_irt_full();
    free(ecam_mem);
    if (fails != 0) {
        printf("vtd_hosttest: %d of %d checks failed\n", fails, checks);
        return fails > 255 ? 255 : fails;
    }
    printf("vtd_hosttest: ok: %d checks\n", checks);
    return 0;
}
