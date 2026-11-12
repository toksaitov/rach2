/*
 * intremap.c: interrupt remapping. Each unit has a table of remapping
 * entries; an I/O APIC's redirection entry or a device's message then names
 * an entry by its index, and the entry says which vector goes to which
 * processor, and from which requester id alone it is taken.
 *
 * The units refuse compatibility-format interrupts, which name a vector and
 * a processor themselves and would pass the table by, as Linux has them
 * (GCMD.CFI clear). So that no interrupt is lost on the way, remapping
 * starts with them allowed, every redirection entry and message already
 * enabled is rewritten to name an entry of its own, and only then are they
 * refused. A message is remapped with no subhandle (SHV clear), so its data
 * is not read and the one write of its address moves it from one format to
 * the other; a redirection entry moves with the one write of its high half.
 *
 * The entries name a processor by its xAPIC id, or, in the x2APIC format
 * (IRTA.EIME), by its 32-bit x2APIC id, which Linux 6.12.115 chooses when
 * the processor offers x2APIC mode, the DMAR table does not opt out of it
 * and every unit has the format (`choose_eim`). The entries take the boot
 * processor's whole id, which the compatibility format gives only the low
 * eight bits of (`irte_dest`). On bare metal the local APIC enters x2APIC
 * mode only then (`try_to_enable_x2apic`, arch/x86/kernel/apic/apic.c,
 * lines 1820 to 1857), and last, as Linux's enable_IR_x2apic moves it
 * (lines 1913 to 1926): once every step that can fail is done, the
 * compatibility format is refused, and the 8259 and the I/O APIC's lines
 * are masked (`intremap_start`). Until then the processor is in xAPIC
 * mode, and the compatibility-format interrupts reach it as they always
 * did (iommu/Readme.md).
 */

#include "vtd.h"

#include "../../../klog.h"
#include "../../../pci.h"
#include "../ioapic.h"

#define IRTE_PRESENT 1ull

/* The remappable message's address: its format bit, and the index's two
 * fields (VT-d specification, 5.1.5.2). */
#define MSI_REMAP_FORMAT (1u << 4)

/* IRTA's size field, 2^(S+1) entries, and its extended interrupt mode
 * enable, the entries' x2APIC format. */
#define IRTA_SIZE 7u
#define IRTA_EIME (1u << 11)

/* Whether remapping is on, and whether its entries are in the x2APIC
 * format. */
static int ir_on;
static int eim;

/** A free entry of `u`'s table, marked taken, or -1. */
static int irte_take(struct vtd_unit *u)
{
    unsigned int i;

    for (i = 0; i < VTD_IRT_ENTRIES; i++) {
        if ((u->irte_used[i / 8] & (1u << (i % 8))) == 0) {
            u->irte_used[i / 8] |= (uint8_t)(1u << (i % 8));
            return (int)i;
        }
    }
    klog(KLOG_ERR, "vtd", "dmar%d: interrupt remapping: table full", u->index);
    return -1;
}

/**
 * Write entry `index` of `u`'s table, the high half first so that the
 * present bit comes last, and drop the unit's cached copy of it.
 */
static void irte_set(struct vtd_unit *u, int index, uint64_t lo, uint64_t hi)
{
    vtd_write_table(u, &u->irt[2 * index + 1], hi);
    vtd_write_table(u, &u->irt[2 * index], lo);
    (void)vtd_inv_iec(u, index);
}

/** Clear entry `index` of `u`'s table and give it back. */
static void irte_clear(struct vtd_unit *u, int index)
{
    vtd_write_table(u, &u->irt[2 * index], 0);
    (void)vtd_inv_iec(u, index);
    vtd_write_table(u, &u->irt[2 * index + 1], 0);
    u->irte_used[index / 8] &= (uint8_t)~(1u << (index % 8));
}

/**
 * The low half of a remapping entry (5.1.4): the vector, the processor's
 * APIC id, and the delivery mode, trigger, destination mode and redirection
 * hint of the format it replaces. The id is in bits 47:40, xAPIC's place,
 * or in the x2APIC format the whole of bits 63:32, Linux's IRTE_DEST
 * (irq_remapping.c, line 61).
 */
static uint64_t irte_low(unsigned int vector, unsigned int apic, unsigned int level,
                         unsigned int dlm, unsigned int dm, unsigned int rh)
{
    uint64_t dest = eim ? (uint64_t)apic : (uint64_t)((apic & 0xFFu) << 8);

    return IRTE_PRESENT | ((uint64_t)(dm & 1u) << 2) | ((uint64_t)(rh & 1u) << 3) |
           ((uint64_t)(level & 1u) << 4) | ((uint64_t)(dlm & 7u) << 5) |
           ((uint64_t)(vector & 0xFFu) << 16) | (dest << 32);
}

/**
 * The APIC id of the processor a compatibility-format interrupt names by
 * the eight bits `dest8`: in the x2APIC format, the boot processor's whole
 * id when its low eight bits are these, since every interrupt goes to it
 * and that format's message and redirection entry hold no more. Linux's
 * prepare_irte writes the whole 32-bit destination the same way
 * (irq_remapping.c, lines 61 and 1107).
 */
static unsigned int irte_dest(unsigned int dest8)
{
    uint32_t boot = vtd_hw_apic_id();

    return (eim && (boot & 0xFFu) == dest8) ? (unsigned int)boot : dest8;
}

/**
 * The requester id a message from (bus, devfn) carries, as the bridges
 * above it rewrite it: `*svt` 1 with `*sid` the id to match, or 2 with
 * `*sid` the bus range, first bus in its high byte and last in its low.
 * This is Linux's set_msi_sid over the walk of pci_for_each_dma_alias: a
 * conventional PCI bridge, or a PCI-to-PCI Express bridge, puts its own id
 * on what passes it, and a PCI Express-to-PCI bridge its secondary bus with
 * devfn 0; the topmost such bridge decides. An id of the bridge's own is
 * matched whole; a secondary bus's, which the device behind it may also
 * carry as itself, only by the range of buses from there to the device's.
 */
static void msi_source(uint8_t bus, uint8_t devfn, unsigned int *svt, uint16_t *sid)
{
    pci_device_t br;
    uint16_t alias = (uint16_t)((bus << 8) | devfn);
    uint8_t at = bus, from = bus;
    int guard;

    for (guard = 0; at != 0 && guard < 256 && pci_find_bridge_for_bus(at, &br); guard++) {
        uint8_t exp = pci_find_capability(br.bus, br.device, br.function, PCI_CAP_ID_PCIE);
        unsigned int type = 0;

        if (exp != 0) {
            type =
                (pci_read_config16(br.bus, br.device, br.function, (uint8_t)(exp + 2)) >> 4) & 0xFu;
        }
        if (exp == 0 || type == 0x8u) {
            alias = (uint16_t)((br.bus << 8) | (br.device << 3) | (br.function & 7u));
            from = br.bus;
        } else if (type == 0x7u) {
            alias = (uint16_t)(at << 8);
            from = br.bus;
        }
        at = br.bus;
    }
    if ((alias >> 8) != from) {
        *svt = 2;
        *sid = (uint16_t)((alias & 0xFF00u) | bus);
    } else {
        *svt = 1;
        *sid = alias;
    }
}

/** The unit of a handle, and its entry. */
static struct vtd_unit *handle_unit(int handle)
{
    int ui = handle >> 16;

    return (ui >= 0 && ui < vtd_nunits) ? &vtd_units[ui] : NULL;
}

/**
 * The remapper's `map` for a message of `dev`: an entry of its unit for
 * the compatibility-format (address, data), verifying the requester id
 * the bridges above it give, and the remapped address and data.
 */
static int msi_map(const pci_device_t *dev, uint32_t address, uint32_t data, uint32_t *raddress,
                   uint32_t *rdata)
{
    uint8_t devfn = (uint8_t)((dev->device << 3) | (dev->function & 7u));
    int ui = dmar_unit_for(&vtd_dmar, dev->bus, devfn), index;
    struct vtd_unit *u;
    unsigned int svt, dest;
    uint16_t sid;

    if (ui < 0 || ui >= vtd_nunits || vtd_units[ui].irt == NULL) {
        klog(KLOG_ERR, "vtd", "msi: %x:%x.%u under no unit that remaps", (unsigned int)dev->bus,
             (unsigned int)dev->device, (unsigned int)dev->function);
        return -1;
    }
    u = &vtd_units[ui];
    index = irte_take(u);
    if (index < 0) {
        return -1;
    }
    msi_source(dev->bus, devfn, &svt, &sid);
    dest = irte_dest((address >> 12) & 0xFFu);
    irte_set(u, index,
             irte_low(data & 0xFFu, dest, (data >> 15) & 1u, (data >> 8) & 7u, (address >> 2) & 1u,
                      (address >> 3) & 1u),
             (uint64_t)sid | ((uint64_t)svt << 18));
    klog(KLOG_DEBUG, "vtd",
         "irte: dmar%d index %d: msi of %x:%x.%u, vector %u to apic %u, %s, sid 0x%04x svt %u", ui,
         index, (unsigned int)dev->bus, (unsigned int)dev->device, (unsigned int)dev->function,
         data & 0xFFu, dest, ((data >> 15) & 1u) ? "level" : "edge", (unsigned int)sid, svt);
    *raddress = 0xFEE00000u | ((uint32_t)(index & 0x7FFF) << 5) | MSI_REMAP_FORMAT |
                ((uint32_t)((index >> 15) & 1) << 2);
    *rdata = 0;
    return (ui << 16) | index;
}

/**
 * The remapper's `map` for redirection entry (low, high) of the I/O APIC of
 * id `id`: an entry of the unit whose scope lists it, verifying its
 * requester id, and the high half of the remapped redirection entry: the
 * index in bits 63:49 and the format bit 48 (5.1.5.1). The low half keeps
 * its vector, polarity, trigger and mask; its fixed delivery and physical
 * destination are zero bits, as the remapped format wants them.
 */
static int ioapic_map(uint8_t id, uint32_t low, uint32_t high, uint32_t *rhigh)
{
    struct vtd_unit *u;
    unsigned int dest;
    uint16_t sid;
    int ui, index;

    if (dmar_ioapic_source(&vtd_dmar, id, &ui, &sid) != 0 || ui >= vtd_nunits ||
        vtd_units[ui].irt == NULL || (low & 0xF00u) != 0) {
        klog(KLOG_ERR, "vtd", "ioapic: id %u under no unit that remaps", (unsigned int)id);
        return -1;
    }
    u = &vtd_units[ui];
    index = irte_take(u);
    if (index < 0) {
        return -1;
    }
    dest = irte_dest(high >> 24);
    irte_set(u, index, irte_low(low & 0xFFu, dest, (low >> 15) & 1u, 0, 0, 0),
             (uint64_t)sid | (1ull << 18));
    klog(KLOG_DEBUG, "vtd",
         "irte: dmar%d index %d: ioapic %u, vector %u to apic %u, %s, sid 0x%04x svt 1", ui, index,
         (unsigned int)id, low & 0xFFu, dest, ((low >> 15) & 1u) ? "level" : "edge",
         (unsigned int)sid);
    *rhigh = ((uint32_t)(index & 0x7FFF) << 17) | (1u << 16);
    return (ui << 16) | index;
}

/** The remappers' `unmap`. */
static void entry_unmap(int handle)
{
    struct vtd_unit *u = handle_unit(handle);

    if (u != NULL && u->irt != NULL) {
        irte_clear(u, handle & 0xFFFF);
    }
}

static const struct pci_msi_remap msi_ops = {msi_map, entry_unmap};
static const struct ioapic_remap ioapic_ops = {ioapic_map, entry_unmap};

/**
 * Whether the entries take the x2APIC format, as Linux's
 * intel_prepare_irq_remapping decides it (irq_remapping.c, lines 738 to
 * 754): the local APIC is or may be in x2APIC mode, the DMAR table does not
 * opt out of it, unless `intremap=no_x2apic_optout` overrides it as on
 * Linux, and every unit has the format (ECAP.EIM).
 */
static int choose_eim(void)
{
    int i;

    if (!vtd_hw_x2apic_possible()) {
        return 0;
    }
    if ((vtd_dmar.flags & DMAR_X2APIC_OUT) != 0 && !vtd_hw_option("intremap", "no_x2apic_optout")) {
        klog(KLOG_INFO, "vtd",
             "interrupt remapping: xapic format: the firmware opts out of x2apic");
        return 0;
    }
    for (i = 0; i < vtd_nunits; i++) {
        if (!VTD_ECAP_EIM(vtd_units[i].ecap)) {
            klog(KLOG_INFO, "vtd", "interrupt remapping: xapic format: dmar%d has no x2apic format",
                 i);
            return 0;
        }
    }
    return 1;
}

/** Turn remapping off on every unit and free its table. */
static void units_off(void)
{
    int i;

    for (i = 0; i < vtd_nunits; i++) {
        struct vtd_unit *u = &vtd_units[i];

        if (u->irt == NULL) {
            continue;
        }
        vtd_set_gcmd(u, VTD_GCMD_IRE, 0);
        vtd_set_gcmd(u, VTD_GCMD_CFI, 0);
        vtd_hw_page_free(u->irt);
        u->irt = NULL;
        u->irt_phys = 0;
    }
}

/**
 * Turn interrupt remapping on, as the file's head describes: 0, or -1 with
 * a record and every interrupt as it was, the local APIC's mode among
 * them, when the table does not offer it, a unit lacks it or the queue, an
 * I/O APIC is in no unit's scope, or an entry could not be had.
 *
 * Every step that can fail comes before the move to x2APIC mode, as in
 * Linux's enable_IR_x2apic (arch/x86/kernel/apic/apic.c, lines 1913 to
 * 1926): the tables are taken, then the units remap with the compatibility
 * format still allowed, the messages are rewritten, and, with interrupts
 * off and the 8259 and every I/O APIC line masked, the redirection entries
 * are rewritten, the compatibility format is refused and only then is the
 * APIC moved. A failure therefore never takes the APIC back through the
 * disabled state, which would drop what its request register holds.
 */
int intremap_start(void)
{
    struct vtd_lines held;
    uint8_t ids[16];
    int i, n;

    if ((vtd_dmar.flags & DMAR_INTR_REMAP) == 0) {
        klog(KLOG_INFO, "vtd", "interrupt remapping: off: the firmware does not offer it");
        return -1;
    }
    for (i = 0; i < vtd_nunits; i++) {
        if (!VTD_ECAP_IR(vtd_units[i].ecap) || vtd_units[i].iq == NULL) {
            klog(KLOG_INFO, "vtd", "interrupt remapping: off: dmar%d cannot", i);
            return -1;
        }
    }
    n = vtd_hw_ioapics(ids, 16);
    for (i = 0; i < n; i++) {
        int ui;
        uint16_t sid;

        if (dmar_ioapic_source(&vtd_dmar, ids[i], &ui, &sid) != 0 || ui >= vtd_nunits) {
            klog(KLOG_WARN, "vtd", "interrupt remapping: off: ioapic %u is under no unit",
                 (unsigned int)ids[i]);
            return -1;
        }
    }
    eim = choose_eim();
    for (i = 0; i < vtd_nunits; i++) {
        struct vtd_unit *u = &vtd_units[i];

        u->irt = (uint64_t *)vtd_hw_page(&u->irt_phys);
        if (u->irt == NULL) {
            units_off();
            return -1;
        }
        vtd_flush_entry(u, u->irt, 4096);
    }
    for (i = 0; i < vtd_nunits; i++) {
        struct vtd_unit *u = &vtd_units[i];

        vtd_hw_write64(u->regs, VTD_IRTA, u->irt_phys | IRTA_SIZE | (eim ? IRTA_EIME : 0u));
        vtd_gcmd_once(u, VTD_GCMD_SIRTP);
        (void)vtd_inv_iec(u, -1);
        vtd_set_gcmd(u, VTD_GCMD_CFI, 1);
        vtd_set_gcmd(u, VTD_GCMD_IRE, 1);
    }
    ir_on = 1;
    if (pci_msi_remap_start(&msi_ops) != 0) {
        ir_on = 0;
        units_off();
        return -1;
    }
    vtd_hw_lines_mask(&held);
    if (ioapic_remap_start(&ioapic_ops) != 0) {
        vtd_hw_lines_restore(&held);
        pci_msi_remap_stop();
        ir_on = 0;
        units_off();
        return -1;
    }
    for (i = 0; i < vtd_nunits; i++) {
        vtd_set_gcmd(&vtd_units[i], VTD_GCMD_CFI, 0);
    }
    if (eim) {
        (void)vtd_hw_x2apic_enable(); /* choose_eim asked that it can */
    }
    vtd_hw_lines_restore(&held);
    klog(KLOG_INFO, "vtd", "interrupt remapping: on, %s format, compatibility format refused",
         eim ? "x2apic" : "xapic");
    return 0;
}

/**
 * Turn interrupt remapping off: compatibility-format interrupts allowed
 * again, every redirection entry and message rewritten in that format and
 * its entry given back, and the units' remapping off. The processors stay
 * in the APIC mode they are in, which the compatibility format reaches by
 * their 8-bit ids.
 */
void intremap_stop(void)
{
    int i;

    if (!ir_on) {
        return;
    }
    for (i = 0; i < vtd_nunits; i++) {
        vtd_set_gcmd(&vtd_units[i], VTD_GCMD_CFI, 1);
    }
    pci_msi_remap_stop();
    ioapic_remap_stop();
    units_off();
    ir_on = 0;
    klog(KLOG_INFO, "vtd", "interrupt remapping: off");
}

int intremap_active(void)
{
    return ir_on;
}

int intremap_x2apic(void)
{
    return ir_on && eim;
}
