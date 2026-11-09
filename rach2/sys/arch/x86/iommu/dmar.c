/*
 * dmar.c: the ACPI DMAR table, which lists the VT-d units, the devices each
 * one translates, and the memory the firmware has devices reach by DMA.
 *
 * The table (VT-d specification, 8) is a header of 48 bytes, the host
 * address width and the flags among them, and a list of structures, each a
 * type and a length first: a unit (DRHD, type 0) with its base and device
 * scopes, and a reserved region (RMRR, type 1) with its range and scopes.
 * A device scope names a device by a start bus and a path of (device,
 * function) pairs, each pair but the last a bridge to the next bus; this
 * file walks the path through the bridges as the firmware numbered them.
 * The other structures (ATSR, RHSA, ANDD, SATC) are passed over.
 */

#include "vtd.h"

#include "../../../klog.h"
#include "../../../lib/kstring.h"

#define DMAR_HEADER 48 /* the ACPI header, the width, the flags, reserved */

#define DMAR_TYPE_DRHD 0
#define DMAR_TYPE_RMRR 1

/** The little-endian 16-bit value at `p`. */
static uint16_t le16(const uint8_t *p)
{
    return (uint16_t)(p[0] | (p[1] << 8));
}

/** The little-endian 32-bit value at `p`. */
static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/** The little-endian 64-bit value at `p`. */
static uint64_t le64(const uint8_t *p)
{
    return (uint64_t)le32(p) | ((uint64_t)le32(p + 4) << 32);
}

/**
 * Walk the device scope at `p`, of `len` bytes, into `out`: 0, or -1 for a
 * scope too short for its own path. A path through a device that is not a
 * bridge leaves the scope naming nothing (bus and devfn 0xFF), as Linux
 * leaves a scope whose device is absent.
 */
static int scope_walk(const uint8_t *p, unsigned int len, struct dmar_scope *out)
{
    unsigned int hops, i;
    uint8_t bus;

    if (len < 8 || p[1] < 8 || p[1] > len || ((p[1] - 6u) & 1u) != 0) {
        return -1;
    }
    out->type = p[0];
    out->enum_id = p[4];
    out->sec = 1;
    out->sub = 0;
    bus = p[5];
    hops = (p[1] - 6u) / 2u;
    for (i = 0; i < hops; i++) {
        uint8_t devfn = (uint8_t)(((p[6 + 2 * i] & 0x1Fu) << 3) | (p[7 + 2 * i] & 7u));
        uint8_t sec, sub;

        if (i + 1 == hops) {
            out->bus = bus;
            out->devfn = devfn;
            break;
        }
        if (vtd_hw_bridge(bus, devfn, &sec, &sub) != 0) {
            out->bus = 0xFF;
            out->devfn = 0xFF;
            return 0;
        }
        bus = sec;
    }
    if (out->type == 2 && vtd_hw_bridge(out->bus, out->devfn, &out->sec, &out->sub) != 0) {
        out->sec = 1;
        out->sub = 0;
    }
    return 0;
}

/**
 * Walk the scopes in [p, end) into `scopes`, at most `max`: their count, or
 * -1 for a malformed one. Scopes past `max` are dropped with a record.
 */
static int scopes_walk(const uint8_t *p, const uint8_t *end, struct dmar_scope *scopes, int max)
{
    int n = 0;

    while (p < end) {
        unsigned int left = (unsigned int)(end - p);

        if (left < 2 || p[1] < 2 || p[1] > left) {
            return -1;
        }
        if (n == max) {
            klog(KLOG_WARN, "vtd", "dmar: scopes past %d dropped", max);
            break;
        }
        if (scope_walk(p, left, &scopes[n]) != 0) {
            return -1;
        }
        n++;
        p += p[1];
    }
    return n;
}

/**
 * Read the DMAR table of `len` bytes at `table` into `out`: 0, or -1 with a
 * record when its signature, length or checksum is wrong or a structure in
 * it is malformed. A unit or region past the limits is left out with a
 * record; a unit of another segment is kept, and `vtd_start` passes it
 * over, since configuration space here is segment 0's alone.
 */
int dmar_parse(const uint8_t *table, uint32_t len, struct dmar_info *out)
{
    const uint8_t *p, *end;
    uint8_t sum = 0;
    uint32_t i;

    k_memset(out, 0, sizeof(*out));
    if (table == NULL || len < DMAR_HEADER || table[0] != 'D' || table[1] != 'M' ||
        table[2] != 'A' || table[3] != 'R' || le32(table + 4) != len) {
        klog(KLOG_WARN, "vtd", "dmar: refused: not a DMAR table");
        return -1;
    }
    for (i = 0; i < len; i++) {
        sum = (uint8_t)(sum + table[i]);
    }
    if (sum != 0) {
        klog(KLOG_WARN, "vtd", "dmar: refused: bad checksum");
        return -1;
    }
    out->haw = (uint8_t)(table[36] + 1u);
    out->flags = table[37];
    p = table + DMAR_HEADER;
    end = table + len;
    while (p < end) {
        unsigned int left = (unsigned int)(end - p);
        unsigned int type, slen;

        if (left < 4) {
            break;
        }
        type = le16(p);
        slen = le16(p + 2);
        if (slen < 4 || slen > left) {
            klog(KLOG_WARN, "vtd", "dmar: refused: structure of type %u has length %u", type, slen);
            return -1;
        }
        if (type == DMAR_TYPE_DRHD) {
            struct dmar_unit_desc *u;
            int n;

            if (slen < 16) {
                return -1;
            }
            if (out->nunits == VTD_MAX_UNITS) {
                klog(KLOG_WARN, "vtd", "dmar: units past %d left out", VTD_MAX_UNITS);
            } else {
                u = &out->units[out->nunits];
                u->flags = p[4];
                u->segment = le16(p + 6);
                u->base = le64(p + 8);
                n = scopes_walk(p + 16, p + slen, u->scopes, VTD_MAX_SCOPES);
                if (n < 0) {
                    klog(KLOG_WARN, "vtd", "dmar: refused: a unit's scope is malformed");
                    return -1;
                }
                u->nscopes = n;
                out->nunits++;
            }
        } else if (type == DMAR_TYPE_RMRR) {
            struct dmar_rmrr *r;
            int n;

            if (slen < 24) {
                return -1;
            }
            if (out->nrmrrs == VTD_MAX_RMRRS) {
                klog(KLOG_WARN, "vtd", "dmar: reserved regions past %d left out", VTD_MAX_RMRRS);
            } else {
                r = &out->rmrrs[out->nrmrrs];
                r->segment = le16(p + 6);
                r->base = le64(p + 8);
                r->end = le64(p + 16);
                n = scopes_walk(p + 24, p + slen, r->scopes, VTD_MAX_RMRR_SCOPES);
                if (n < 0 || r->end < r->base) {
                    klog(KLOG_WARN, "vtd", "dmar: refused: a reserved region is malformed");
                    return -1;
                }
                r->nscopes = n;
                out->nrmrrs++;
            }
        }
        p += slen;
    }
    return 0;
}

/** Whether `s` names the PCI device at (bus, devfn): it is the device, or a
 * bridge the device is below. */
static int scope_has(const struct dmar_scope *s, uint8_t bus, uint8_t devfn)
{
    if (s->type != 1 && s->type != 2) {
        return 0;
    }
    if (s->bus == bus && s->devfn == devfn) {
        return 1;
    }
    return s->type == 2 && bus >= s->sec && bus <= s->sub;
}

/**
 * The unit that translates the device at (bus, devfn) of segment 0: one
 * whose scope names it, or else the segment's unit with INCLUDE_PCI_ALL.
 * Its index in `d->units`, or -1.
 */
int dmar_unit_for(const struct dmar_info *d, uint8_t bus, uint8_t devfn)
{
    int i, j, all = -1;

    for (i = 0; i < d->nunits; i++) {
        const struct dmar_unit_desc *u = &d->units[i];

        if (u->segment != 0) {
            continue;
        }
        if ((u->flags & DMAR_INCLUDE_ALL) != 0) {
            if (all < 0) {
                all = i;
            }
            continue;
        }
        for (j = 0; j < u->nscopes; j++) {
            if (scope_has(&u->scopes[j], bus, devfn)) {
                return i;
            }
        }
    }
    return all;
}

/** Whether reserved region `r` is for the device at (bus, devfn). */
int dmar_rmrr_names(const struct dmar_rmrr *r, uint8_t bus, uint8_t devfn)
{
    int j;

    if (r->segment != 0) {
        return 0;
    }
    for (j = 0; j < r->nscopes; j++) {
        if (scope_has(&r->scopes[j], bus, devfn)) {
            return 1;
        }
    }
    return 0;
}

/**
 * The unit whose scope lists the I/O APIC of id `id`, in `*unit`, and the
 * requester id its messages carry, in `*sid`: 0, or -1 when no unit lists
 * it.
 */
int dmar_ioapic_source(const struct dmar_info *d, uint8_t id, int *unit, uint16_t *sid)
{
    int i, j;

    for (i = 0; i < d->nunits; i++) {
        const struct dmar_unit_desc *u = &d->units[i];

        for (j = 0; j < u->nscopes; j++) {
            if (u->scopes[j].type == 3 && u->scopes[j].enum_id == id) {
                *unit = i;
                *sid = (uint16_t)((u->scopes[j].bus << 8) | u->scopes[j].devfn);
                return 0;
            }
        }
    }
    return -1;
}

/** Log the table as Linux's `DMAR:` lines give it, at DEBUG. */
void dmar_report(const struct dmar_info *d)
{
    int i, j;

    klog(KLOG_DEBUG, "vtd", "dmar: host address width %d, flags 0x%x", (int)d->haw,
         (unsigned int)d->flags);
    for (i = 0; i < d->nunits; i++) {
        const struct dmar_unit_desc *u = &d->units[i];

        klog(KLOG_DEBUG, "vtd", "dmar: unit at 0x%lx, segment %d, flags 0x%x",
             (unsigned long)u->base, (int)u->segment, (unsigned int)u->flags);
        for (j = 0; j < u->nscopes; j++) {
            const struct dmar_scope *s = &u->scopes[j];

            if (s->type == 3 || s->type == 4) {
                klog(KLOG_DEBUG, "vtd", "dmar: %s id %d at %x:%x.%d",
                     s->type == 3 ? "ioapic" : "hpet", (int)s->enum_id, (unsigned int)s->bus,
                     (unsigned int)(s->devfn >> 3), (int)(s->devfn & 7u));
            } else {
                klog(KLOG_DEBUG, "vtd", "dmar: %s %x:%x.%d", s->type == 2 ? "bridge" : "device",
                     (unsigned int)s->bus, (unsigned int)(s->devfn >> 3), (int)(s->devfn & 7u));
            }
        }
    }
    for (i = 0; i < d->nrmrrs; i++) {
        klog(KLOG_DEBUG, "vtd", "dmar: reserved region 0x%lx to 0x%lx",
             (unsigned long)d->rmrrs[i].base, (unsigned long)d->rmrrs[i].end);
    }
}
