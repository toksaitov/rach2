/*
 * pcimsi_hosttest.c: `pci_msi_enable` against a model of configuration space.
 *
 * host-deps: rach2/sys/pci.c rach2/sys/arch/x86/pciconf.c
 *
 * The order of the writes is the whole of this: a device told to send
 * messages while its address is half written sends them somewhere, and a
 * device left with MSI-X enabled beside MSI is undefined by the
 * specification. No machine can be made to show either, so the model
 * records every write in order and the rows read that record.
 *
 * Configuration space is reached through the two ports, which is where the
 * model sits: the driver's own `pci_read_config32` and its neighbours are
 * the code under test, not a stub.
 */

#include "../../rach2/sys/pci.h"

/* Where a PC writes a message, as arch/x86/pciconf.c builds it: the local
 * APIC's page, with the processor's identity in bits 12 to 19. */
#define PCI_MSI_ADDRESS_BASE 0xFEE00000u

extern int printf(const char *fmt, ...);

/* ---- the model ---- */

#define CONFIG_BYTES 256

static unsigned char space[CONFIG_BYTES];
static unsigned int address_port;
static unsigned int writes[64];
static unsigned int written[64];
static int nwrites;

/* One device: a capability list whose contents each row lays out. */
static void model_reset(void)
{
    int i;

    for (i = 0; i < CONFIG_BYTES; i++) {
        space[i] = 0;
    }
    space[0] = 0x86; /* vendor 0x8086 */
    space[1] = 0x80;
    space[6] = 0x10; /* the capability-list bit in the status word */
    space[0x34] = 0; /* no list unless a row builds one */
    nwrites = 0;
}

/* A capability of `id` at `offset`, linked after `prev` (0x34 for the
   first), with `len` bytes of room. */
static void model_cap(unsigned char offset, unsigned char id, unsigned char prev)
{
    space[prev == 0x34 ? 0x34 : (unsigned char)(prev + 1)] = offset;
    space[offset] = id;
    space[offset + 1] = 0;
}

void outd(unsigned short port, unsigned int value)
{
    if (port == 0xCF8) {
        address_port = value;
        return;
    }
    if (port == 0xCFC) {
        unsigned int off = address_port & 0xFC;
        int i;

        if (off == 0x04) {
            /* The dword at 4 is the command word and the status word, and the
               status word's faults are cleared by writing them as one. A
               device model that stored the whole dword would let a driver
               that reads and writes it back keep every latched bit, which is
               the defect the row below is for. */
            unsigned int status = (unsigned int)space[0x06] | ((unsigned int)space[0x07] << 8);

            status &= ~((value >> 16) & 0xF900u);
            space[0x04] = (unsigned char)value;
            space[0x05] = (unsigned char)(value >> 8);
            space[0x06] = (unsigned char)status;
            space[0x07] = (unsigned char)(status >> 8);
            if (nwrites < 64) {
                writes[nwrites] = off;
                written[nwrites] = value;
                nwrites++;
            }
            return;
        }
        for (i = 0; i < 4; i++) {
            space[off + i] = (unsigned char)(value >> (8 * i));
        }
        if (nwrites < 64) {
            writes[nwrites] = off;
            written[nwrites] = value;
            nwrites++;
        }
    }
}

unsigned int ind(unsigned short port)
{
    unsigned int off = address_port & 0xFC;

    if (port != 0xCFC) {
        return 0xFFFFFFFFu;
    }
    return (unsigned int)space[off] | ((unsigned int)space[off + 1] << 8) |
           ((unsigned int)space[off + 2] << 16) | ((unsigned int)space[off + 3] << 24);
}

void outb(unsigned short port, unsigned char value)
{
    (void)port;
    (void)value;
}

unsigned char inb(unsigned short port)
{
    (void)port;
    return 0xFF;
}

/* A halfword access goes to the data port the register's own offset selects,
   0xCFC or 0xCFE, and touches those two bytes and no others. The model
   records it in the same order record as a dword write, under the dword
   offset, so a row can ask which came first. */
void outw(unsigned short port, unsigned short value)
{
    unsigned int off;

    if (port != 0xCFC && port != 0xCFE) {
        return;
    }
    off = (address_port & 0xFC) + (unsigned int)(port - 0xCFC);
    space[off] = (unsigned char)value;
    space[off + 1] = (unsigned char)(value >> 8);
    if (nwrites < 64) {
        writes[nwrites] = address_port & 0xFC;
        written[nwrites] = (unsigned int)value << (8 * (unsigned int)(port - 0xCFC));
        nwrites++;
    }
}

unsigned short inw(unsigned short port)
{
    unsigned int off;

    if (port != 0xCFC && port != 0xCFE) {
        return 0xFFFF;
    }
    off = (address_port & 0xFC) + (unsigned int)(port - 0xCFC);
    return (unsigned short)((unsigned int)space[off] | ((unsigned int)space[off + 1] << 8));
}

/* The kernel takes each access with interrupts off, which on a host with no
   interrupts is a pair of counters: a row can then require that every write
   this driver makes was taken inside one. */
static int irq_depth;
static int irq_unbalanced;

unsigned int kirq_save(void)
{
    irq_depth++;
    return 0;
}

void kirq_restore(unsigned int flags)
{
    (void)flags;
    if (irq_depth == 0) {
        irq_unbalanced++;
    } else {
        irq_depth--;
    }
}

void klog(int level, const char *subsystem, const char *fmt, ...)
{
    (void)level;
    (void)subsystem;
    (void)fmt;
}

static unsigned int the_apic_id;

unsigned int lapic_id(void)
{
    return the_apic_id;
}

/* ---- the rows ---- */

static int fails;

static void check(int ok, const char *what)
{
    if (!ok) {
        fails++;
        printf("pcimsi_hosttest: FAIL: %s\n", what);
    }
}

static unsigned short cfg16(unsigned char off)
{
    return (unsigned short)(space[off] | (space[off + 1] << 8));
}

static unsigned int cfg32(unsigned char off)
{
    return (unsigned int)space[off] | ((unsigned int)space[off + 1] << 8) |
           ((unsigned int)space[off + 2] << 16) | ((unsigned int)space[off + 3] << 24);
}

/* The index in the write record of the first write that touched `off`, or
   -1: the dword offset is what the model records. */
static int first_write_of(unsigned char off)
{
    int i;

    for (i = 0; i < nwrites; i++) {
        if (writes[i] == (unsigned int)(off & 0xFC)) {
            return i;
        }
    }
    return -1;
}

/* The first write to the dword at `off` whose value has `bits` set in the
   halfword at `off`, or -1. */
static int first_write_with(unsigned char off, unsigned int bits)
{
    unsigned int shift = 8u * (off & 2u);
    int i;

    for (i = 0; i < nwrites; i++) {
        if (writes[i] == (unsigned int)(off & 0xFC) && ((written[i] >> shift) & bits) == bits) {
            return i;
        }
    }
    return -1;
}

/*
 * MSI-X, whose table is in the device's memory and not in its configuration
 * space: the model's table is an array the rows read after the call. The
 * device comes in with MSI enabled by firmware and entry 0 masked, as a
 * device out of reset has it, with a stale address in it. These are the
 * last rows, so they end with the check that every access of the run left
 * the interrupt flag as it found it.
 */
static void msix_rows(const pci_device_t *dev)
{
    volatile unsigned int table[8];
    unsigned char bar = 0xFF;
    unsigned int offset = 0, entries = 0;
    int i;

    model_reset();
    model_cap(0x60, PCI_CAP_ID_MSI, 0x34);
    space[0x62] = 0x80 | (unsigned char)PCI_MSI_ENABLE;
    model_cap(0xA0, PCI_CAP_ID_MSIX, 0x60);
    space[0xA2] = 0x01; /* two entries */
    space[0xA4] = 0x04; /* the table at 0x3000 of BAR 4 */
    space[0xA5] = 0x30;
    for (i = 0; i < 8; i++) {
        table[i] = 0xDEADBEEFu;
    }
    table[3] = 1; /* entry 0 masked */
    the_apic_id = 2;
    check(pci_msix_table(dev, &bar, &offset, &entries) == 0, "the MSI-X table is found");
    check(bar == 4 && offset == 0x3000 && entries == 2,
          "at the BAR, offset and size the capability gives");
    nwrites = 0;
    check(pci_msix_enable(dev, (uintptr_t)table, 54) == 0, "a device with MSI-X is programmed");
    check(table[0] == (PCI_MSI_ADDRESS_BASE | (2u << 12)) && table[1] == 0,
          "entry 0's address is the APIC's page with the processor in it");
    check(table[2] == 54, "its data is the vector");
    check(table[3] == 0, "and its own mask is off");
    check(table[4] == 0xDEADBEEFu, "the other entry is left alone");
    check((cfg16(0x62) & PCI_MSI_ENABLE) == 0, "MSI is disabled");
    check((cfg16(0xA2) & PCI_MSIX_ENABLE) != 0 && (cfg16(0xA2) & PCI_MSIX_MASK_ALL) == 0,
          "MSI-X is enabled with the function mask off");
    {
        int msi_down = first_write_of(0x60), masked = first_write_with(0xA2, 0xC000u), last = -1;

        for (i = 0; i < nwrites; i++) {
            if (writes[i] == 0xA0) {
                last = i;
            }
        }
        check(msi_down >= 0 && masked >= 0 && msi_down < masked,
              "MSI goes down before MSI-X is enabled");
        check(masked >= 0 && last > masked && ((written[last] >> 16) & 0x4000u) == 0,
              "MSI-X is enabled masked first and unmasked by the last write");
    }

    model_reset();
    model_cap(0x60, PCI_CAP_ID_MSI, 0x34);
    space[0x62] = 0x80 | (unsigned char)PCI_MSI_ENABLE;
    model_cap(0xA0, PCI_CAP_ID_MSIX, 0x60);
    space[0xA2] = 0x01;
    table[3] = 1;
    check(pci_msix_hold(dev) == 0, "a device with MSI-X is held");
    check((cfg16(0x62) & PCI_MSI_ENABLE) == 0, "with MSI disabled");
    check((cfg16(0xA2) & PCI_MSIX_ENABLE) != 0 && (cfg16(0xA2) & PCI_MSIX_MASK_ALL) != 0,
          "and MSI-X enabled with the function mask on");
    check(table[3] == 1, "and its table untouched");

    model_reset();
    model_cap(0x60, PCI_CAP_ID_MSI, 0x34);
    space[0x62] = 0x80;
    check(pci_msix_hold(dev) == -1 && nwrites == 0, "a device without MSI-X is not held");
    check(pci_msix_table(dev, &bar, &offset, &entries) == -1,
          "a device without MSI-X has no table");
    check(pci_msix_enable(dev, (uintptr_t)table, 55) == -1, "and is refused");
    check(nwrites == 0, "with nothing written");

    model_reset();
    model_cap(0xF8, PCI_CAP_ID_MSIX, 0x34);
    check(pci_msix_table(dev, &bar, &offset, &entries) == -1,
          "an MSI-X capability that runs past the end of its space is refused");
    check(pci_msix_enable(dev, (uintptr_t)table, 56) == -1 && nwrites == 0,
          "and nothing is written");

    check(irq_depth == 0 && irq_unbalanced == 0,
          "every access left the interrupt flag as it found it");
}

int main(void)
{
    pci_device_t dev;
    int rc;

    dev.bus = 0;
    dev.device = 4;
    dev.function = 0;
    the_apic_id = 0;

    /* A 32-bit capability, which QEMU's HDA is not and the specification
       still allows: the address at 4 and the data at 8. The device comes in
       as firmware might leave it, with its enable bit set and four of its
       eight vectors in use, and both must be down before anything else is
       written. */
    model_reset();
    model_cap(0x50, PCI_CAP_ID_MSI, 0x34);
    space[0x52] = 0x07; /* eight vectors offered, 32-bit */
    space[0x53] = 0x00;
    space[0x52] |= (uint8_t)PCI_MSI_ENABLE; /* and enabled already */
    space[0x53] |= 0x02;                    /* with four of them in use */
    rc = pci_msi_enable(&dev, 48);
    check(rc == 0, "a device with a 32-bit MSI capability is programmed");
    check(cfg32(0x54) == (PCI_MSI_ADDRESS_BASE | 0), "the address is the APIC's page");
    check(cfg16(0x58) == 48, "the data is the vector, at offset 8");
    check((cfg16(0x52) & PCI_MSI_ENABLE) != 0, "and the enable bit is set");
    check((cfg16(0x52) & PCI_MSI_MULTIPLE_ENABLE) == 0, "with one vector to use");
    check((cfg16(0x52) & 0x000Eu) == 0x0006u,
          "and the count of vectors the device offers left as it was");
    {
        int enable_down = -1, i;

        /* The enable bit and the count of vectors go down in the same first
           write, before the address: a device left enabled by firmware would
           otherwise send from the address it already had. */
        for (i = 0; i < nwrites; i++) {
            if (writes[i] == 0x50 && (written[i] & (PCI_MSI_ENABLE << 16)) == 0 &&
                enable_down < 0) {
                enable_down = i;
            }
        }
        check(enable_down == 0, "the enable bit is taken down by the first write of all");
    }
    {
        int enable_at = -1, address_at = first_write_of(0x54), data_at = first_write_of(0x58), i;

        /* The first write that sets the enable bit: a later one would hide
           an enable that came before the address. */
        for (i = 0; i < nwrites; i++) {
            if (writes[i] == 0x50 && (written[i] & (PCI_MSI_ENABLE << 16)) != 0 && enable_at < 0) {
                enable_at = i;
            }
        }
        check(address_at >= 0 && data_at >= 0 && enable_at >= 0 && address_at < enable_at &&
                  data_at < enable_at,
              "the address and the data are written before the enable bit");
        check(first_write_of(0x50) >= 0 && address_at >= 0 && first_write_of(0x50) < address_at,
              "and the enable bit is cleared before either");
    }

    /* A 64-bit capability, which is what both controllers this kernel drives
       have: the upper half zero and the data at 12. The upper half comes in
       with a stale address in it, because a device left programmed for a
       64-bit destination and given a 32-bit one would send to neither. */
    model_reset();
    model_cap(0x60, PCI_CAP_ID_MSI, 0x34);
    space[0x62] = 0x80; /* one vector, 64-bit address */
    space[0x6B] = 0x12; /* and an upper half somebody else wrote */
    the_apic_id = 3;
    check(pci_msi_enable(&dev, 49) == 0, "a device with a 64-bit MSI capability is programmed");
    check(cfg32(0x64) == (PCI_MSI_ADDRESS_BASE | (3u << 12)),
          "the address has the processor's identity in it");
    check(cfg32(0x68) == 0, "the upper half of the address is zero");
    check(cfg16(0x6C) == 49, "the data is the vector, at offset 12");
    check((cfg16(0x62) & PCI_MSI_ENABLE) != 0, "and the enable bit is set");
    {
        int enable_at = -1, address_at = first_write_of(0x64), upper_at = first_write_of(0x68),
            data_at = first_write_of(0x6C), i;

        for (i = 0; i < nwrites; i++) {
            if (writes[i] == 0x60 && (written[i] & (PCI_MSI_ENABLE << 16)) != 0 && enable_at < 0) {
                enable_at = i;
            }
        }
        check(address_at >= 0 && upper_at >= 0 && data_at >= 0 && enable_at >= 0 &&
                  address_at < enable_at && upper_at < enable_at && data_at < enable_at,
              "both halves of the address and the data are written before the enable bit");
    }

    /* A device that can mask its own vectors, left with this one masked. It
       would be programmed, enabled and silent. */
    model_reset();
    model_cap(0x60, PCI_CAP_ID_MSI, 0x34);
    space[0x62] = 0x80;                               /* 64-bit */
    space[0x63] = (uint8_t)(PCI_MSI_PER_VECTOR >> 8); /* and it can mask */
    space[0x70] = 0xFF;                               /* every vector masked */
    the_apic_id = 0;
    check(pci_msi_enable(&dev, 50) == 0, "a device that masks its own vectors is programmed");
    check(cfg32(0x70) == 0, "with the mask taken off");
    {
        int mask_at = first_write_of(0x70), enable_at = -1, i;

        for (i = 0; i < nwrites; i++) {
            if (writes[i] == 0x60 && (written[i] & (PCI_MSI_ENABLE << 16)) != 0 && enable_at < 0) {
                enable_at = i;
            }
        }
        check(mask_at >= 0 && enable_at >= 0 && mask_at < enable_at,
              "and the mask taken off before the enable bit is set");
    }

    /* A capability so near the end of configuration space that its own
       registers would run past it. An offset is one byte, so the writes
       would wrap onto the identity registers at zero. */
    model_reset();
    model_cap(0xF8, PCI_CAP_ID_MSI, 0x34);
    space[0xFA] = 0x80; /* 64-bit, so its data would be at 0x104 */
    check(pci_msi_enable(&dev, 51) == -1,
          "a capability that runs past the end of its space is refused");
    check(cfg32(0x00) == 0x80868086u || cfg32(0x00) == 0x00008086u,
          "and nothing was written over the identity registers");

    /* A device whose firmware left MSI-X enabled: it is disabled first. */
    model_reset();
    model_cap(0x60, PCI_CAP_ID_MSI, 0x34);
    space[0x62] = 0x80;
    model_cap(0x90, PCI_CAP_ID_MSIX, 0x60);
    space[0x92] = 0x0F;
    space[0x93] = 0x80; /* MSI-X enabled */
    the_apic_id = 0;
    check(pci_msi_enable(&dev, 50) == 0, "a device with MSI-X enabled beside MSI is programmed");
    check((cfg16(0x92) & PCI_MSIX_ENABLE) == 0, "with MSI-X disabled");
    check((cfg16(0x92) & 0x07FFu) == 0x000Fu, "and the rest of its control word left alone");
    {
        int msix_at = first_write_of(0x90), address_at = first_write_of(0x64);

        check(msix_at >= 0 && address_at >= 0 && msix_at < address_at,
              "MSI-X is disabled before the address is written");
    }

    /* The devices this refuses. */
    model_reset();
    check(pci_msi_enable(&dev, 52) == -1, "a device whose capability list is empty is refused");
    check(nwrites == 0, "and nothing is written");

    model_reset();
    model_cap(0x90, PCI_CAP_ID_MSIX, 0x34);
    space[0x92] = 0x0F;
    check(pci_msi_enable(&dev, 53) == -1, "a device with MSI-X and no MSI is refused");
    check(nwrites == 0, "and nothing is written");

    /* Disabling clears the enable bit and leaves the address and data. */
    model_reset();
    model_cap(0x60, PCI_CAP_ID_MSI, 0x34);
    space[0x62] = 0x80;
    (void)pci_msi_enable(&dev, 53);
    nwrites = 0;
    pci_msi_disable(&dev);
    check((cfg16(0x62) & PCI_MSI_ENABLE) == 0, "disabling clears the enable bit");
    check(cfg32(0x64) == PCI_MSI_ADDRESS_BASE && cfg16(0x6C) == 53,
          "and leaves the address and the data as they were");

    model_reset();
    pci_msi_disable(&dev);
    check(nwrites == 0, "disabling a device with no MSI writes nothing");

    /* A halfword written into the dword that holds the command word must not
       take the status word beside it with it: the status word's bits are
       cleared by writing them as one, and a read of the dword and a write of
       it back would clear every fault the bus had latched. */
    model_reset();
    space[0x04] = 0x03; /* the command word: I/O and memory decoding */
    space[0x05] = 0x00;
    space[0x06] = 0x10; /* the status word: a capability list, and */
    space[0x07] = 0x40; /* a system error the bus latched */
    pci_write_config16(dev.bus, dev.device, dev.function, 0x04, 0x0007u);
    check(cfg16(0x04) == 0x0007u, "a halfword write reaches the register it names");
    check(cfg16(0x06) == 0x4010u, "and leaves the latched status beside it alone");

    msix_rows(&dev);

    if (fails == 0) {
        printf("pcimsi_hosttest: OK\n");
        return 0;
    }
    printf("pcimsi_hosttest: %d failure(s)\n", fails);
    return 1;
}
