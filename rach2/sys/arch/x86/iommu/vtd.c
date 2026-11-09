/*
 * vtd.c: the VT-d units, their root and context tables, the domains and
 * their second-level page tables, and invalidation, by the queue where a
 * unit has one and by its registers where it does not.
 *
 * A unit's translation stays off until one of its devices is given to a
 * domain, so the host's own drivers run with physical DMA addresses. The
 * first device given makes every other device of the unit a pass-through
 * context entry, or an entry of an identity domain where the unit has no
 * pass-through, before translation turns on; the last one given back turns
 * it off again. A unit whose walks are not coherent with the processor's
 * caches (ECAP.C clear) has each table entry written back from the cache
 * after it is written, and a unit without snoop control (ECAP.SC clear)
 * gets no snoop bit in its domains' leaves.
 */

#include "vtd.h"

#include "../../../iommu.h"
#include "../../../klog.h"
#include "../../../lib/kerrno.h"
#include "../../../lib/kstring.h"
#include "../../../mm/kheap.h"

struct dmar_info vtd_dmar;
struct vtd_unit vtd_units[VTD_MAX_UNITS];
int vtd_nunits;

static int vtd_up;      /* a unit is ready */
static int vtd_dma_off; /* intel_iommu=off: no device is given to a domain */

/* Domains, context entries and the units' translation, across processors.
 * Taken before a unit's own lock, never after it. */
static struct spinlock vtd_lock = SPINLOCK_INIT;

/* How long a unit has to finish a command: a million polls a microsecond
 * apart. */
#define VTD_WAIT_US 1000000u

/* The invalidation descriptors' types and fields (VT-d specification,
 * 6.5.2). */
#define QI_CC        0x1ull /* context cache */
#define QI_IOTLB     0x2ull
#define QI_IEC       0x4ull /* interrupt entry cache */
#define QI_WAIT      0x5ull
#define QI_GLOBAL    (1ull << 4)
#define QI_DOMAIN    (2ull << 4)
#define QI_DEVICE    (3ull << 4)
#define QI_IOTLB_DW  (1ull << 6)
#define QI_IOTLB_DR  (1ull << 7)
#define QI_IEC_INDEX (1ull << 4)
#define QI_WAIT_SW   (1ull << 5)

/* The register-based commands' fields (11.4.4 and 11.4.8.1). */
#define CCMD_ICC     (1ull << 63)
#define CCMD_GLOBAL  (1ull << 61)
#define CCMD_DEVICE  (3ull << 61)
#define IOTLB_IVT    (1ull << 63)
#define IOTLB_GLOBAL (1ull << 60)
#define IOTLB_DOMAIN (2ull << 60)
#define IOTLB_DR     (1ull << 49)
#define IOTLB_DW     (1ull << 48)

/** Read the 32-bit register at `off` of `u`. */
static uint32_t rd32(const struct vtd_unit *u, unsigned int off)
{
    return vtd_hw_read32(u->regs, off);
}

/** Read the 64-bit register at `off` of `u`. */
static uint64_t rd64(const struct vtd_unit *u, unsigned int off)
{
    return vtd_hw_read64(u->regs, off);
}

/** Write the 32-bit register at `off` of `u`. */
static void wr32(const struct vtd_unit *u, unsigned int off, uint32_t v)
{
    vtd_hw_write32(u->regs, off, v);
}

/** Write the 64-bit register at `off` of `u`. */
static void wr64(const struct vtd_unit *u, unsigned int off, uint64_t v)
{
    vtd_hw_write64(u->regs, off, v);
}

/**
 * Wait until the bits `mask` of the global status are `want`: 0, or -1
 * with a record after a second.
 */
static int wait_status(const struct vtd_unit *u, uint32_t mask, uint32_t want, const char *what)
{
    unsigned int i;

    for (i = 0; i < VTD_WAIT_US; i++) {
        if ((rd32(u, VTD_GSTS) & mask) == want) {
            return 0;
        }
        vtd_hw_udelay(1);
    }
    klog(KLOG_ERR, "vtd", "dmar%d: %s: no answer, status 0x%x", u->index, what, rd32(u, VTD_GSTS));
    return -1;
}

/**
 * Turn the persistent command bit `bit` (TE, QIE, IRE or CFI) on or off,
 * keeping the others as they were set, and wait for the status to follow.
 * The status register holds the one-shot bits too, so the command written
 * is the persistent bits kept in `u->gcmd`, never the status read back.
 * CFI is waited for a millisecond and no longer, without a record, since
 * QEMU's unit leaves its status clear whatever the command says.
 */
void vtd_set_gcmd(struct vtd_unit *u, uint32_t bit, int on)
{
    unsigned int i;

    if (on) {
        u->gcmd |= bit;
    } else {
        u->gcmd &= ~bit;
    }
    wr32(u, VTD_GCMD, u->gcmd);
    if (bit != VTD_GCMD_CFI) {
        (void)wait_status(u, bit, on ? bit : 0u, "command");
        return;
    }
    for (i = 0; i < 1000u && (rd32(u, VTD_GSTS) & bit) != (on ? bit : 0u); i++) {
        vtd_hw_udelay(1);
    }
}

/** Issue the one-shot command `bit` (SRTP, SIRTP or WBF) beside the
 * persistent bits, and wait for its status: set for a pointer, clear for
 * the write buffer's flush. */
void vtd_gcmd_once(struct vtd_unit *u, uint32_t bit)
{
    wr32(u, VTD_GCMD, u->gcmd | bit);
    (void)wait_status(u, bit, bit == VTD_GCMD_WBF ? 0u : bit, "command");
}

/** Write back what the unit reads at [p, p + len) when its walks do not
 * snoop the processor's caches. */
void vtd_flush_entry(const struct vtd_unit *u, const void *p, unsigned long len)
{
    if (!VTD_ECAP_C(u->ecap)) {
        vtd_hw_flush(p, len);
    }
}

/** Write `value` to the table entry at `slot` of a unit's own table and make
 * it visible to the unit. */
void vtd_write_table(const struct vtd_unit *u, uint64_t *slot, uint64_t value)
{
    *slot = value;
    vtd_flush_entry(u, slot, sizeof(*slot));
}

/** Flush the write buffer of a unit that has one (CAP.RWBF), which a unit
 * needs after its tables change from not present to present. */
static void wbflush(struct vtd_unit *u)
{
    if (VTD_CAP_RWBF(u->cap)) {
        vtd_gcmd_once(u, VTD_GCMD_WBF);
    }
}

/**
 * Put the descriptor (lo, hi) on `u`'s queue followed by a wait descriptor
 * that writes a new sequence number, and wait until the unit has written
 * it: 0, or -1 with a record when the unit refused the descriptor (IQE) or
 * did not finish within a second. A refused descriptor is overwritten by
 * the wait, as Linux's qi_check_fault does, so the queue goes on. Called
 * with `u->lock` held.
 */
static int qi_submit(struct vtd_unit *u, uint64_t lo, uint64_t hi)
{
    unsigned int at = u->iq_tail, wait_at = (at + 1u) % 256u, i;
    uint64_t wlo, whi;
    int bad = 0;

    u->iq_seq++;
    wlo = QI_WAIT | QI_WAIT_SW | ((uint64_t)u->iq_seq << 32);
    whi = u->iq_status_phys;
    u->iq[2 * at] = lo;
    u->iq[2 * at + 1] = hi;
    u->iq[2 * wait_at] = wlo;
    u->iq[2 * wait_at + 1] = whi;
    u->iq_tail = (wait_at + 1u) % 256u;
    wr64(u, VTD_IQT, (uint64_t)u->iq_tail << 4);
    for (i = 0; i < VTD_WAIT_US; i++) {
        if (*u->iq_status == u->iq_seq) {
            return bad ? -1 : 0;
        }
        if ((rd32(u, VTD_FSTS) & VTD_FSTS_IQE) != 0) {
            unsigned int head = (unsigned int)(rd64(u, VTD_IQH) >> 4) & 0xFFu;

            klog(KLOG_ERR, "vtd", "dmar%d: invalidation refused: 0x%lx 0x%lx", u->index,
                 (unsigned long)u->iq[2 * head], (unsigned long)u->iq[2 * head + 1]);
            u->iq[2 * head] = wlo;
            u->iq[2 * head + 1] = whi;
            wr32(u, VTD_FSTS, VTD_FSTS_IQE);
            u->errors++;
            bad = 1;
        }
        vtd_hw_udelay(1);
    }
    klog(KLOG_ERR, "vtd", "dmar%d: invalidation: no answer", u->index);
    u->errors++;
    return -1;
}

/** Wait until the register command at `off` clears its bit `busy`. */
static int reg_command(struct vtd_unit *u, unsigned int off, uint64_t cmd, uint64_t busy)
{
    unsigned int i;

    wr64(u, off, cmd);
    for (i = 0; i < VTD_WAIT_US; i++) {
        if ((rd64(u, off) & busy) == 0) {
            return 0;
        }
        vtd_hw_udelay(1);
    }
    klog(KLOG_ERR, "vtd", "dmar%d: invalidation: no answer", u->index);
    u->errors++;
    return -1;
}

/**
 * Invalidate `u`'s context cache: all of it when `sid` is negative, else
 * the entry of requester id `sid`, tagged `did`.
 */
static int inv_context(struct vtd_unit *u, int sid, unsigned int did)
{
    uint32_t flags;
    int r;

    if (u->iq != NULL) {
        uint64_t lo = QI_CC | (sid < 0 ? QI_GLOBAL : QI_DEVICE);

        if (sid >= 0) {
            lo |= ((uint64_t)did << 16) | ((uint64_t)(unsigned int)sid << 32);
        }
        flags = spin_lock_irqsave(&u->lock);
        r = qi_submit(u, lo, 0);
        spin_unlock_irqrestore(&u->lock, flags);
        return r;
    }
    flags = spin_lock_irqsave(&u->lock);
    if (sid < 0) {
        r = reg_command(u, VTD_CCMD, CCMD_ICC | CCMD_GLOBAL, CCMD_ICC);
    } else {
        r = reg_command(u, VTD_CCMD,
                        CCMD_ICC | CCMD_DEVICE | ((uint64_t)(unsigned int)sid << 16) | did,
                        CCMD_ICC);
    }
    spin_unlock_irqrestore(&u->lock, flags);
    return r;
}

/**
 * Invalidate `u`'s IOTLB: all of it when `did` is 0, else domain `did`'s
 * entries, draining the reads and writes in flight where the unit can.
 */
static int inv_iotlb(struct vtd_unit *u, unsigned int did)
{
    uint32_t flags;
    int r;

    if (u->iq != NULL) {
        uint64_t lo = QI_IOTLB | (did == 0 ? QI_GLOBAL : QI_DOMAIN) | ((uint64_t)did << 16);

        if (VTD_CAP_DRD(u->cap)) {
            lo |= QI_IOTLB_DR;
        }
        if (VTD_CAP_DWD(u->cap)) {
            lo |= QI_IOTLB_DW;
        }
        flags = spin_lock_irqsave(&u->lock);
        r = qi_submit(u, lo, 0);
        spin_unlock_irqrestore(&u->lock, flags);
        return r;
    }
    {
        uint64_t cmd = IOTLB_IVT | (did == 0 ? IOTLB_GLOBAL : IOTLB_DOMAIN) | ((uint64_t)did << 32);

        if (VTD_CAP_DRD(u->cap)) {
            cmd |= IOTLB_DR;
        }
        if (VTD_CAP_DWD(u->cap)) {
            cmd |= IOTLB_DW;
        }
        flags = spin_lock_irqsave(&u->lock);
        r = reg_command(u, VTD_ECAP_IRO(u->ecap) + 8u, cmd, IOTLB_IVT);
        spin_unlock_irqrestore(&u->lock, flags);
    }
    return r;
}

/**
 * Invalidate `u`'s interrupt entry cache: all of it when `index` is
 * negative, else the one entry. Only the queue can; a unit that remaps
 * interrupts has one.
 */
int vtd_inv_iec(struct vtd_unit *u, int index)
{
    uint64_t lo = QI_IEC;
    uint32_t flags;
    int r;

    if (u->iq == NULL) {
        return -1;
    }
    if (index >= 0) {
        lo |= QI_IEC_INDEX | ((uint64_t)(unsigned int)index << 32);
    }
    flags = spin_lock_irqsave(&u->lock);
    r = qi_submit(u, lo, 0);
    spin_unlock_irqrestore(&u->lock, flags);
    return r;
}

/* ---- domains ---- */

/** The index into a table of `level` (1 the leaves) for `iova`. */
static unsigned int level_index(uint64_t iova, int level)
{
    return (unsigned int)(iova >> (12 + 9 * (level - 1))) & 511u;
}

/** Write a domain's table entry and make it visible to its units. */
static void dom_write(const struct iommu_domain *d, uint64_t *slot, uint64_t value)
{
    *slot = value;
    if (d->flush) {
        vtd_hw_flush(slot, sizeof(*slot));
    }
}

/**
 * The entry of `d` at level `level` for `iova`, making the tables above it
 * when `create` is set: its address, or NULL when a table is missing (or
 * could not be made). A large page on the way stops the walk at its entry,
 * whose level goes to `*found`.
 */
static uint64_t *dom_slot(struct iommu_domain *d, uint64_t iova, int level, int create, int *found)
{
    uint64_t *table = d->top;
    int l;

    for (l = d->levels; l > level; l--) {
        uint64_t *slot = &table[level_index(iova, l)];

        if ((*slot & (VTD_PTE_R | VTD_PTE_W)) == 0) {
            uint64_t phys;
            uint64_t *next;

            if (!create) {
                return NULL;
            }
            next = (uint64_t *)vtd_hw_page(&phys);
            if (next == NULL) {
                return NULL;
            }
            if (d->flush) {
                vtd_hw_flush(next, 4096);
            }
            dom_write(d, slot, phys | VTD_PTE_R | VTD_PTE_W);
        } else if ((*slot & VTD_PTE_PS) != 0) {
            if (found != NULL) {
                *found = l;
            }
            return slot;
        }
        table = (uint64_t *)vtd_hw_virt(*slot & VTD_PTE_ADDR);
    }
    if (found != NULL) {
        *found = level;
    }
    return &table[level_index(iova, level)];
}

/** Free the tables below `table`, of level `level`, and `table` itself. */
static void dom_free_tables(uint64_t *table, int level)
{
    unsigned int i;

    if (level > 1) {
        for (i = 0; i < 512; i++) {
            if ((table[i] & (VTD_PTE_R | VTD_PTE_W)) != 0 && (table[i] & VTD_PTE_PS) == 0) {
                dom_free_tables((uint64_t *)vtd_hw_virt(table[i] & VTD_PTE_ADDR), level - 1);
            }
        }
    }
    vtd_hw_page_free(table);
}

/** Write every table of `d` back from the cache, when a unit that walks
 * incoherently first takes the domain. */
static void dom_flush_tables(const uint64_t *table, int level)
{
    unsigned int i;

    vtd_hw_flush(table, 4096);
    if (level == 1) {
        return;
    }
    for (i = 0; i < 512; i++) {
        if ((table[i] & (VTD_PTE_R | VTD_PTE_W)) != 0 && (table[i] & VTD_PTE_PS) == 0) {
            dom_flush_tables((const uint64_t *)vtd_hw_virt(table[i] & VTD_PTE_ADDR), level - 1);
        }
    }
}

/** A domain with `levels` levels of tables, the top's first entry made so
 * that a unit with one level fewer can walk from it. */
static struct iommu_domain *dom_new(int levels)
{
    struct iommu_domain *d = (struct iommu_domain *)kzalloc(sizeof(*d));

    if (d == NULL) {
        return NULL;
    }
    d->levels = levels;
    d->limit = (uint64_t)1 << (12 + 9 * levels);
    d->top = (uint64_t *)vtd_hw_page(&d->top_phys);
    if (d->top == NULL) {
        kfree(d);
        return NULL;
    }
    if (levels == 4 && dom_slot(d, 0, 3, 1, NULL) == NULL) {
        vtd_hw_page_free(d->top);
        kfree(d);
        return NULL;
    }
    return d;
}

/** After `d` changed from not present to present: what each of its units
 * needs to see it, an IOTLB invalidation in caching mode and a write-buffer
 * flush otherwise. */
static void dom_mapped(struct iommu_domain *d)
{
    int i;

    for (i = 0; i < vtd_nunits; i++) {
        if (d->did[i] == 0) {
            continue;
        }
        if (VTD_CAP_CM(vtd_units[i].cap)) {
            (void)inv_iotlb(&vtd_units[i], d->did[i]);
        } else {
            wbflush(&vtd_units[i]);
        }
    }
}

/** After `d` lost pages: each of its units' IOTLB entries for it go. */
static void dom_unmapped(struct iommu_domain *d)
{
    int i;

    for (i = 0; i < vtd_nunits; i++) {
        if (d->did[i] != 0) {
            (void)inv_iotlb(&vtd_units[i], d->did[i]);
        }
    }
}

/** Clear the 4 KiB pages of [iova, iova + size) in `d`; called with
 * `vtd_lock` held. */
static void dom_clear(struct iommu_domain *d, uint64_t iova, uint64_t size)
{
    uint64_t at;

    for (at = iova; at < iova + size; at += 4096) {
        int found = 1;
        uint64_t *slot = dom_slot(d, at, 1, 0, &found);

        if (slot != NULL && found == 1 && (*slot & (VTD_PTE_R | VTD_PTE_W)) != 0) {
            dom_write(d, slot, 0);
            d->pages--;
        }
    }
}

/* What `dom_map` adds to IOMMU_READ and IOMMU_WRITE: leaves of 2 MiB, and a
 * page already mapped passed over rather than refused. */
#define DOM_LARGE 0x100
#define DOM_SKIP  0x200

/**
 * Map [iova, iova + size) to [phys, ...) in `d` with `prot`, in 4 KiB
 * leaves or, with DOM_LARGE, 2 MiB ones; a page already mapped is
 * -EEXIST, or passed over with DOM_SKIP. Called with `vtd_lock` held; what
 * a failed call mapped is cleared again.
 */
static int dom_map(struct iommu_domain *d, uint64_t iova, uint64_t phys, uint64_t size, int prot)
{
    int level = (prot & DOM_LARGE) ? 2 : 1, skip = (prot & DOM_SKIP) != 0;
    uint64_t step = level == 2 ? 0x200000ull : 4096ull, at, leaf;

    leaf = (prot & IOMMU_READ ? VTD_PTE_R : 0) | (prot & IOMMU_WRITE ? VTD_PTE_W : 0);
    if (d->snoop) {
        leaf |= VTD_PTE_SNP;
    }
    if (level == 2) {
        leaf |= VTD_PTE_PS;
    }
    for (at = 0; at < size; at += step) {
        int found = level;
        uint64_t *slot = dom_slot(d, iova + at, level, 1, &found);

        if (slot == NULL) {
            if (level == 1) {
                dom_clear(d, iova, at);
            }
            return -ENOMEM;
        }
        if ((*slot & (VTD_PTE_R | VTD_PTE_W)) != 0 || found != level) {
            if (skip) {
                continue;
            }
            dom_clear(d, iova, at);
            return -EEXIST;
        }
        dom_write(d, slot, ((phys + at) & VTD_PTE_ADDR) | leaf);
        d->pages += (unsigned long)(step / 4096);
    }
    if (iova + size > d->mapped_end) {
        d->mapped_end = iova + size;
    }
    return 0;
}

/**
 * The host's identity domain for `u`, which has no pass-through: every
 * address below the end of RAM, and at least the first 4 GiB, mapped to
 * itself, in 2 MiB pages where the unit has them, and each reserved region
 * of the unit's devices; NULL when memory is short.
 */
static struct iommu_domain *identity_new(struct vtd_unit *u)
{
    struct iommu_domain *d = dom_new(u->levels);
    uint64_t top = vtd_hw_ram_top();
    int i, large = (VTD_CAP_SLLPS(u->cap) & 1u) != 0, r;

    if (d == NULL) {
        return NULL;
    }
    d->identity = 1;
    d->flush = !VTD_ECAP_C(u->ecap);
    if (top < 0x100000000ull) {
        top = 0x100000000ull;
    }
    top = (top + 0x1FFFFFull) & ~0x1FFFFFull;
    r = dom_map(d, 0, 0, top, IOMMU_READ | IOMMU_WRITE | DOM_SKIP | (large ? DOM_LARGE : 0));
    for (i = 0; r == 0 && i < vtd_dmar.nrmrrs; i++) {
        const struct dmar_rmrr *m = &vtd_dmar.rmrrs[i];
        uint64_t base = m->base & ~0xFFFull, end = (m->end | 0xFFFull) + 1u;

        r = dom_map(d, base, base, end - base, IOMMU_READ | IOMMU_WRITE | DOM_SKIP);
    }
    if (r != 0) {
        dom_free_tables(d->top, d->levels);
        kfree(d);
        return NULL;
    }
    d->did[u->index] = VTD_DID_HOST;
    return d;
}

/* ---- context entries ---- */

/** The context table of `bus` on `u`, made when `create` is set: NULL when
 * there is none or memory is short. */
static uint64_t *ctx_table(struct vtd_unit *u, unsigned int bus, int create)
{
    uint64_t phys;

    if (u->ctx[bus] != NULL || !create) {
        return u->ctx[bus];
    }
    u->ctx[bus] = (uint64_t *)vtd_hw_page(&phys);
    if (u->ctx[bus] == NULL) {
        return NULL;
    }
    vtd_flush_entry(u, u->ctx[bus], 4096);
    vtd_write_table(u, &u->root[2 * bus + 1], 0);
    vtd_write_table(u, &u->root[2 * bus], phys | 1u);
    return u->ctx[bus];
}

/** The table `u` walks for `d`: its top, or below it for a unit of one
 * level fewer; 0 when `u` cannot walk `d`. */
static uint64_t walk_root(const struct vtd_unit *u, const struct iommu_domain *d)
{
    if (u->levels == d->levels) {
        return d->top_phys;
    }
    if (u->levels + 1 == d->levels) {
        return d->top[0] & VTD_PTE_ADDR;
    }
    return 0;
}

/** The host's context entry for a device of `u`: pass-through, or the
 * identity domain. */
static void host_entry(const struct vtd_unit *u, uint64_t *lo, uint64_t *hi)
{
    *hi = (uint64_t)(u->levels - 2) | ((uint64_t)VTD_DID_HOST << 8);
    if (u->identity == NULL) {
        *lo = ((uint64_t)VTD_TT_PASSTHROUGH << 2) | 1u;
    } else {
        *lo = walk_root(u, u->identity) | 1u;
    }
}

/**
 * Set the context entry of (bus, devfn) on `u` to (lo, hi). A present
 * entry is cleared first and its caches, tagged `old_did`, invalidated, so
 * that the unit never sees half of each; then the new entry is written,
 * high half first, and made visible: invalidated in caching mode, where the
 * unit may cache a not-present entry, and by a write-buffer flush
 * otherwise. Called with `vtd_lock` held.
 */
static int ctx_set(struct vtd_unit *u, uint8_t bus, uint8_t devfn, uint64_t lo, uint64_t hi,
                   unsigned int old_did)
{
    uint64_t *t = ctx_table(u, bus, 1);
    uint64_t *e;
    int sid = (bus << 8) | devfn;

    if (t == NULL) {
        return -ENOMEM;
    }
    e = &t[2 * devfn];
    if ((e[0] & 1u) != 0) {
        vtd_write_table(u, &e[0], 0);
        (void)inv_context(u, sid, old_did);
        (void)inv_iotlb(u, old_did);
    }
    vtd_write_table(u, &e[1], hi);
    vtd_write_table(u, &e[0], lo);
    if (VTD_CAP_CM(u->cap)) {
        (void)inv_context(u, sid, 0);
        (void)inv_iotlb(u, (unsigned int)((hi >> 8) & 0xFFFFu));
    } else {
        wbflush(u);
    }
    return 0;
}

/**
 * Give every device of `u`'s scope the host's context entry, before the
 * unit's translation first turns on: each function of each bus present
 * that the unit translates, so that the requester ids bridges put on their
 * devices' DMA are covered too. Called with `vtd_lock` held.
 */
static int host_build(struct vtd_unit *u)
{
    unsigned int bus, devfn;
    uint64_t lo, hi;

    if (!VTD_ECAP_PT(u->ecap) && u->identity == NULL) {
        u->identity = identity_new(u);
        if (u->identity == NULL) {
            return -ENOMEM;
        }
    }
    host_entry(u, &lo, &hi);
    for (bus = 0; bus < 256; bus++) {
        if (!vtd_hw_bus_present((uint8_t)bus)) {
            continue;
        }
        for (devfn = 0; devfn < 256; devfn++) {
            uint64_t *t;

            if (dmar_unit_for(&vtd_dmar, (uint8_t)bus, (uint8_t)devfn) != u->index) {
                continue;
            }
            t = ctx_table(u, bus, 1);
            if (t == NULL) {
                return -ENOMEM;
            }
            if ((t[2 * devfn] & 1u) == 0) {
                vtd_write_table(u, &t[2 * devfn + 1], hi);
                vtd_write_table(u, &t[2 * devfn], lo);
            }
        }
    }
    (void)inv_context(u, -1, 0);
    (void)inv_iotlb(u, 0);
    wbflush(u);
    u->host_built = 1;
    return 0;
}

/** A free domain id of `u`, marked taken, or 0. */
static uint16_t did_take(struct vtd_unit *u)
{
    unsigned int i;

    for (i = VTD_DID_FIRST; i < u->ndomains; i++) {
        if ((u->did_used[i / 8] & (1u << (i % 8))) == 0) {
            u->did_used[i / 8] |= (uint8_t)(1u << (i % 8));
            return (uint16_t)i;
        }
    }
    return 0;
}

/** Give domain id `did` of `u` back. */
static void did_give(struct vtd_unit *u, unsigned int did)
{
    u->did_used[did / 8] &= (uint8_t)~(1u << (did % 8));
}

/** Whether reserved regions of the device of class `cls` may go unmapped
 * once a VM has it: a USB controller's or a display's, as Linux's
 * device_rmrr_is_relaxable decides. */
static int rmrr_relaxable(uint32_t cls)
{
    return (cls >> 16) == 0x03u || (cls >> 8) == 0x0C03u;
}

/* ---- the interface ---- */

int iommu_present(void)
{
    return vtd_up && !vtd_dma_off;
}

int iommu_unit_of(uint8_t bus, uint8_t devfn)
{
    int i = dmar_unit_for(&vtd_dmar, bus, devfn);

    if (!vtd_up || i < 0 || i >= vtd_nunits || vtd_units[i].regs == 0) {
        return -1;
    }
    return i;
}

/** The domain's levels: four when a unit walks four, so that every unit
 * can, a unit of three walking from the top's first entry. */
struct iommu_domain *iommu_domain_alloc(void)
{
    int i, levels = 3;

    if (!iommu_present()) {
        return NULL;
    }
    for (i = 0; i < vtd_nunits; i++) {
        if (vtd_units[i].levels == 4) {
            levels = 4;
        }
    }
    return dom_new(levels);
}

void iommu_domain_free(struct iommu_domain *d)
{
    if (d == NULL) {
        return;
    }
    dom_free_tables(d->top, d->levels);
    kfree(d);
}

uint64_t iommu_domain_limit(const struct iommu_domain *d)
{
    return d->limit;
}

int iommu_map(struct iommu_domain *d, uint64_t iova, uint64_t phys, uint64_t size, int prot)
{
    uint32_t flags;
    int r;

    if (((iova | phys | size) & 0xFFFu) != 0 || size == 0 || iova + size < iova ||
        iova + size > d->limit) {
        return -EINVAL;
    }
    flags = spin_lock_irqsave(&vtd_lock);
    r = dom_map(d, iova, phys, size, prot & (IOMMU_READ | IOMMU_WRITE));
    if (r == 0) {
        dom_mapped(d);
    }
    spin_unlock_irqrestore(&vtd_lock, flags);
    return r;
}

int iommu_unmap(struct iommu_domain *d, uint64_t iova, uint64_t size)
{
    uint32_t flags;

    if (((iova | size) & 0xFFFu) != 0 || iova + size < iova) {
        return -EINVAL;
    }
    flags = spin_lock_irqsave(&vtd_lock);
    dom_clear(d, iova, size);
    dom_unmapped(d);
    spin_unlock_irqrestore(&vtd_lock, flags);
    return 0;
}

uint64_t iommu_iova_to_phys(const struct iommu_domain *d, uint64_t iova)
{
    const uint64_t *table = d->top;
    int l;

    for (l = d->levels; l >= 1; l--) {
        uint64_t e = table[level_index(iova, l)];

        if ((e & (VTD_PTE_R | VTD_PTE_W)) == 0) {
            return ~0ull;
        }
        if (l == 1 || (e & VTD_PTE_PS) != 0) {
            uint64_t span = (uint64_t)1 << (12 + 9 * (l - 1));

            return (e & VTD_PTE_ADDR & ~(span - 1u)) | (iova & (span - 1u));
        }
        table = (const uint64_t *)vtd_hw_virt(e & VTD_PTE_ADDR);
    }
    return ~0ull;
}

/**
 * Whether the device at (bus, devfn) may leave the host's domain: -EPERM
 * when a reserved region of its scope is one Linux keeps mapped, 0
 * otherwise.
 */
static int rmrr_check(uint8_t bus, uint8_t devfn)
{
    uint32_t cls = vtd_hw_class(bus, devfn);
    int i;

    for (i = 0; i < vtd_dmar.nrmrrs; i++) {
        if (dmar_rmrr_names(&vtd_dmar.rmrrs[i], bus, devfn) && !rmrr_relaxable(cls)) {
            klog(KLOG_WARN, "vtd", "attach: %x:%x.%d refused: its reserved region must stay mapped",
                 (unsigned int)bus, (unsigned int)(devfn >> 3), (int)(devfn & 7u));
            return -EPERM;
        }
    }
    return 0;
}

/**
 * Whether unit `u` can walk `d`: 0 with the table it walks from in
 * `*root` and the end of what it translates in `*limit`, or -EINVAL when
 * the levels differ by more than one, `d` maps past that end, or `d`'s
 * leaves have the snoop bit the unit lacks.
 */
static int walk_check(const struct vtd_unit *u, const struct iommu_domain *d, uint64_t *root,
                      uint64_t *limit)
{
    *root = walk_root(u, d);
    *limit = (uint64_t)1 << (12 + 9 * u->levels);
    if (*limit > ((uint64_t)1 << u->mgaw)) {
        *limit = (uint64_t)1 << u->mgaw;
    }
    if (*root == 0 || d->mapped_end > *limit || (d->snoop && !VTD_ECAP_SC(u->ecap))) {
        return -EINVAL;
    }
    return 0;
}

/** Whether a unit has a device of `d`. */
static int dom_in_use(const struct iommu_domain *d)
{
    int i;

    for (i = 0; i < vtd_nunits; i++) {
        if (d->users[i] != 0) {
            return 1;
        }
    }
    return 0;
}

/** `iommu_attach` for the device at (bus, devfn) of unit `u`, with
 * `vtd_lock` held. */
static int attach_locked(struct iommu_domain *d, struct vtd_unit *u, uint8_t bus, uint8_t devfn)
{
    uint64_t root, limit, *t = ctx_table(u, bus, 0);
    int ui = u->index, r, new_did = 0;

    if (t != NULL && (t[2 * devfn] & 1u) != 0 &&
        ((t[2 * devfn + 1] >> 8) & 0xFFFFu) != VTD_DID_HOST) {
        return -EBUSY;
    }
    if ((r = walk_check(u, d, &root, &limit)) != 0) {
        return r;
    }
    if (d->did[ui] == 0) {
        if ((d->did[ui] = did_take(u)) == 0) {
            return -ENOMEM;
        }
        new_did = 1;
    }
    if (!dom_in_use(d) && d->pages == 0) {
        /* The first unit decides, before a leaf is written. */
        d->snoop = VTD_ECAP_SC(u->ecap) != 0;
    }
    if (!VTD_ECAP_C(u->ecap) && !d->flush) {
        d->flush = 1;
        dom_flush_tables(d->top, d->levels);
    }
    if (!u->host_built) {
        r = host_build(u);
    }
    if (r == 0) {
        r = ctx_set(u, bus, devfn, root | ((uint64_t)VTD_TT_TRANSLATE << 2) | 1u,
                    (uint64_t)(u->levels - 2) | ((uint64_t)d->did[ui] << 8), VTD_DID_HOST);
    }
    if (r != 0) {
        if (new_did) {
            did_give(u, d->did[ui]);
            d->did[ui] = 0;
        }
        return r;
    }
    d->users[ui]++;
    if (limit < d->limit) {
        d->limit = limit;
    }
    u->assigned++;
    if (!u->translating) {
        vtd_set_gcmd(u, VTD_GCMD_TE, 1);
        u->translating = 1;
        klog(KLOG_INFO, "vtd", "dmar%d: translation on", u->index);
    }
    return 0;
}

/**
 * A unit without pass-through has its identity domain made here, before the
 * lock is taken, since a unit without large pages has a million entries to
 * write and the lock holds interrupts off; one made by another attach in the
 * meantime wins, and this one is freed.
 */
int iommu_attach(struct iommu_domain *d, uint8_t bus, uint8_t devfn)
{
    struct iommu_domain *identity = NULL;
    struct vtd_unit *u;
    uint32_t flags;
    int ui = iommu_unit_of(bus, devfn), r;

    if (ui < 0 || vtd_dma_off) {
        return -ENODEV;
    }
    if ((r = rmrr_check(bus, devfn)) != 0) {
        return r;
    }
    u = &vtd_units[ui];
    if (!VTD_ECAP_PT(u->ecap) && u->identity == NULL && (identity = identity_new(u)) == NULL) {
        return -ENOMEM;
    }
    flags = spin_lock_irqsave(&vtd_lock);
    if (identity != NULL && u->identity == NULL) {
        u->identity = identity;
        identity = NULL;
    }
    r = attach_locked(d, u, bus, devfn);
    spin_unlock_irqrestore(&vtd_lock, flags);
    iommu_domain_free(identity);
    if (r == 0) {
        klog(KLOG_INFO, "vtd", "attach: %x:%x.%d to domain %d of dmar%d", (unsigned int)bus,
             (unsigned int)(devfn >> 3), (int)(devfn & 7u), (int)d->did[ui], ui);
    }
    return r;
}

void iommu_detach(struct iommu_domain *d, uint8_t bus, uint8_t devfn)
{
    struct vtd_unit *u;
    uint64_t lo, hi, *t;
    uint32_t flags;
    int ui = iommu_unit_of(bus, devfn);

    if (ui < 0 || d->did[ui] == 0) {
        return;
    }
    u = &vtd_units[ui];
    flags = spin_lock_irqsave(&vtd_lock);
    t = ctx_table(u, bus, 0);
    if (t == NULL || ((t[2 * devfn + 1] >> 8) & 0xFFFFu) != d->did[ui]) {
        spin_unlock_irqrestore(&vtd_lock, flags);
        return;
    }
    host_entry(u, &lo, &hi);
    (void)ctx_set(u, bus, devfn, lo, hi, d->did[ui]);
    if (--d->users[ui] == 0) {
        did_give(u, d->did[ui]);
        d->did[ui] = 0;
    }
    if (--u->assigned == 0 && u->translating) {
        vtd_set_gcmd(u, VTD_GCMD_TE, 0);
        u->translating = 0;
        klog(KLOG_INFO, "vtd", "dmar%d: translation off", u->index);
    }
    spin_unlock_irqrestore(&vtd_lock, flags);
}

int iommu_reserved_regions(uint8_t bus, uint8_t devfn, struct iommu_region *out, int max)
{
    uint32_t cls = vtd_hw_class(bus, devfn);
    int i, n = 0;

    for (i = 0; i < vtd_dmar.nrmrrs && n < max; i++) {
        if (dmar_rmrr_names(&vtd_dmar.rmrrs[i], bus, devfn)) {
            out[n].start = vtd_dmar.rmrrs[i].base;
            out[n].end = vtd_dmar.rmrrs[i].end;
            out[n].type = rmrr_relaxable(cls) ? IOMMU_RESV_DIRECT_RELAXABLE : IOMMU_RESV_DIRECT;
            n++;
        }
    }
    /* An ISA bridge's floppy controller DMAs below 16 MiB (Linux's
     * INTEL_IOMMU_FLOPPY_WA). */
    if (n < max && (cls >> 8) == 0x0601u) {
        out[n].start = 0;
        out[n].end = 0xFFFFFFull;
        out[n].type = IOMMU_RESV_DIRECT_RELAXABLE;
        n++;
    }
    if (n < max) {
        out[n].start = 0xFEE00000ull;
        out[n].end = 0xFEEFFFFFull;
        out[n].type = IOMMU_RESV_MSI;
        n++;
    }
    return n;
}

/* ---- bring-up ---- */

/**
 * Make unit `u`, the table's row `desc`, ready: its registers mapped, what
 * the firmware left on turned off, the root table set and the queue on,
 * translation off. 0, or -1 with a record.
 */
static int unit_start(struct vtd_unit *u, const struct dmar_unit_desc *desc, int index)
{
    uint32_t ver, sts;
    unsigned int nd;

    k_memset(u, 0, sizeof(*u));
    u->index = index;
    u->desc = desc;
    u->regs = vtd_hw_map(desc->base);
    if (u->regs == 0) {
        klog(KLOG_WARN, "vtd", "dmar%d: refused: 0x%lx could not be mapped", index,
             (unsigned long)desc->base);
        return -1;
    }
    ver = rd32(u, VTD_VER);
    u->cap = rd64(u, VTD_CAP);
    u->ecap = rd64(u, VTD_ECAP);
    if (ver == 0xFFFFFFFFu || u->cap == ~0ull) {
        klog(KLOG_WARN, "vtd", "dmar%d: refused: 0x%lx does not answer", index,
             (unsigned long)desc->base);
        u->regs = 0;
        return -1;
    }
    klog(KLOG_INFO, "vtd", "dmar%d: at 0x%lx, version %u.%u, cap 0x%lx, ecap 0x%lx", index,
         (unsigned long)desc->base, (ver >> 4) & 0xFu, ver & 0xFu, (unsigned long)u->cap,
         (unsigned long)u->ecap);
    if ((VTD_CAP_SAGAW(u->cap) & 4u) != 0) {
        u->levels = 4;
    } else if ((VTD_CAP_SAGAW(u->cap) & 2u) != 0) {
        u->levels = 3;
    } else {
        klog(KLOG_WARN, "vtd", "dmar%d: refused: no 3- or 4-level tables", index);
        u->regs = 0;
        return -1;
    }
    u->mgaw = VTD_CAP_MGAW(u->cap);
    nd = 4u + 2u * VTD_CAP_ND(u->cap);
    u->ndomains = nd >= 8u ? VTD_DID_MAX : (1u << nd);
    u->did_used[0] = 3; /* ids 0 and 1 */

    /* What the firmware left on goes off: translation, remapping, the queue
     * once it is empty, and the protected memory regions. */
    sts = rd32(u, VTD_GSTS);
    u->gcmd = sts & (VTD_GCMD_TE | VTD_GCMD_QIE | VTD_GCMD_IRE | VTD_GCMD_CFI);
    if ((sts & VTD_GCMD_TE) != 0) {
        klog(KLOG_WARN, "vtd", "dmar%d: translation was on: turned off", index);
        vtd_set_gcmd(u, VTD_GCMD_TE, 0);
    }
    if ((sts & VTD_GCMD_IRE) != 0) {
        vtd_set_gcmd(u, VTD_GCMD_IRE, 0);
    }
    if ((sts & VTD_GCMD_QIE) != 0) {
        unsigned int i;

        for (i = 0; i < VTD_WAIT_US && rd64(u, VTD_IQH) != rd64(u, VTD_IQT); i++) {
            vtd_hw_udelay(1);
        }
        vtd_set_gcmd(u, VTD_GCMD_QIE, 0);
    }
    if (((u->cap >> 5) & 3u) != 0 && (rd32(u, VTD_PMEN) & 1u) != 0) {
        unsigned int i;

        wr32(u, VTD_PMEN, 0);
        for (i = 0; i < VTD_WAIT_US && (rd32(u, VTD_PMEN) & 1u) != 0; i++) {
            vtd_hw_udelay(1);
        }
    }

    u->root = (uint64_t *)vtd_hw_page(&u->root_phys);
    if (u->root == NULL) {
        u->regs = 0;
        return -1;
    }
    vtd_flush_entry(u, u->root, 4096);
    wr64(u, VTD_RTADDR, u->root_phys);
    vtd_gcmd_once(u, VTD_GCMD_SRTP);

    if (VTD_ECAP_QI(u->ecap)) {
        uint64_t *status;

        u->iq = (uint64_t *)vtd_hw_page(&u->iq_phys);
        status = (uint64_t *)vtd_hw_page(&u->iq_status_phys);
        if (u->iq == NULL || status == NULL) {
            u->regs = 0;
            return -1;
        }
        u->iq_status = (volatile uint32_t *)status;
        wr64(u, VTD_IQT, 0);
        wr64(u, VTD_IQA, u->iq_phys);
        vtd_set_gcmd(u, VTD_GCMD_QIE, 1);
    }
    (void)inv_context(u, -1, 0);
    (void)inv_iotlb(u, 0);
    return 0;
}

/**
 * Read the DMAR table and bring up each unit of segment 0, the fault
 * reports, and, unless the boot line says `intremap=off`, interrupt
 * remapping: the units up, or -1 when there are none.
 */
int vtd_start(void)
{
    const uint8_t *table;
    uint32_t len = 0;
    int i, n = 0;

    table = vtd_hw_dmar(&len);
    if (table == NULL) {
        klog(KLOG_DEBUG, "vtd", "none: no DMAR table");
        return -1;
    }
    if (dmar_parse(table, len, &vtd_dmar) != 0) {
        return -1;
    }
    dmar_report(&vtd_dmar);
    for (i = 0; i < vtd_dmar.nunits; i++) {
        if (vtd_dmar.units[i].segment != 0) {
            klog(KLOG_WARN, "vtd", "dmar: unit of segment %d passed over",
                 (int)vtd_dmar.units[i].segment);
            continue;
        }
        if (unit_start(&vtd_units[n], &vtd_dmar.units[i], n) == 0) {
            n++;
        }
    }
    vtd_nunits = n;
    if (n == 0) {
        return -1;
    }
    vtd_up = 1;
    vtd_dma_off = vtd_hw_option("intel_iommu", "off");
    (void)vtd_fault_start();
    if (vtd_hw_option("intremap", "off")) {
        klog(KLOG_INFO, "vtd", "interrupt remapping: off by intremap=off");
    } else {
        (void)intremap_start();
    }
    klog(KLOG_INFO, "vtd", "ready: %d units, interrupt remapping %s, translation %s", n,
         intremap_active() ? "on" : "off",
         vtd_dma_off ? "off by intel_iommu=off" : "on for assigned devices only");
    return n;
}

void iommu_boot(void)
{
    (void)vtd_start();
}

unsigned long iommu_dma_faults(void)
{
    unsigned long n = 0;
    int i;

    for (i = 0; i < vtd_nunits; i++) {
        n += vtd_units[i].dma_faults;
    }
    return n;
}

unsigned long iommu_irq_faults(void)
{
    unsigned long n = 0;
    int i;

    for (i = 0; i < vtd_nunits; i++) {
        n += vtd_units[i].ir_faults;
    }
    return n;
}

int iommu_irq_remapped(void)
{
    return intremap_active();
}
