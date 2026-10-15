/*
 * pcimsi_hosttest.c: `pci_msi_enable` against a model of configuration space.
 *
 * host-deps: rach2/sys/pci.c rach2/sys/arch/x86/pciconf.c
 *
 * The rows check the order of the writes: a device told to send messages
 * while its address is half written sends them to a wrong address, and the
 * PCI specification leaves a device with MSI-X enabled beside MSI undefined.
 * No machine can be made to show either, so the model records every write
 * in order and the rows read that record.
 *
 * The model stands behind the configuration ports 0xCF8 and 0xCFC, so the
 * accessors of arch/x86/pciconf.c, `pci_read_config32` and the rest, run as
 * they do in the kernel and are part of the code under test.
 */

#include "../../rach2/sys/pci.h"

/* Where a PC writes a message, as arch/x86/pciconf.c builds it: the local
 * APIC's page, with the processor's identity in bits 12 to 19. */
#define PCI_MSI_ADDRESS_BASE 0xFEE00000u

extern int printf(const char *fmt, ...);

/* ---- the model ---- */

#define CONFIG_BYTES 256

/* The configuration space of the one device, and the last value written to
 * the address port 0xCF8. */
static unsigned char space[CONFIG_BYTES];
static unsigned int address_port;
/* The write record, in order: each write's dword offset and its value, the
 * value of a halfword write shifted to its place in the dword. Writes past
 * the 64th are not recorded. */
static unsigned int writes[64];
static unsigned int written[64];
static int nwrites;

/* The MSI-X table of the row that has one, or 0. Each configuration write
 * records entry 0's address beside itself in `table_at`: the model cannot
 * see a store to the table, but it can see where the store falls among the
 * configuration writes. */
static volatile unsigned int *msix_table;
static unsigned int table_at[64];

/**
 * Reset the model to one device, vendor 0x8086, with the capability-list bit
 * set and an empty list, and clear the write record.
 */
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

/**
 * Put a capability of `id` at `offset` at the end of the list, linked from
 * the capability at `prev`, or from the pointer at 0x34 when `prev` is 0x34.
 */
static void model_cap(unsigned char offset, unsigned char id, unsigned char prev)
{
    space[prev == 0x34 ? 0x34 : (unsigned char)(prev + 1)] = offset;
    space[offset] = id;
    space[offset + 1] = 0;
}

/**
 * Take a dword write to the address port 0xCF8 or the data port 0xCFC, and
 * record a data write. The dword at 4 is the command word and the status
 * word, whose bits are cleared by writing them as one; the model applies
 * that rule, so a driver that reads the dword and writes it back clears the
 * latched bits, which the halfword row checks.
 */
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
            unsigned int status = (unsigned int)space[0x06] | ((unsigned int)space[0x07] << 8);

            status &= ~((value >> 16) & 0xF900u);
            space[0x04] = (unsigned char)value;
            space[0x05] = (unsigned char)(value >> 8);
            space[0x06] = (unsigned char)status;
            space[0x07] = (unsigned char)(status >> 8);
            if (nwrites < 64) {
                writes[nwrites] = off;
                written[nwrites] = value;
                table_at[nwrites] = msix_table ? msix_table[0] : 0;
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
            table_at[nwrites] = msix_table ? msix_table[0] : 0;
            nwrites++;
        }
    }
}

/** Read the dword the address port selects from the data port 0xCFC. */
unsigned int ind(unsigned short port)
{
    unsigned int off = address_port & 0xFC;

    if (port != 0xCFC) {
        return 0xFFFFFFFFu;
    }
    return (unsigned int)space[off] | ((unsigned int)space[off + 1] << 8) |
           ((unsigned int)space[off + 2] << 16) | ((unsigned int)space[off + 3] << 24);
}

/** Ignore a byte write: no row writes a byte of configuration space. */
void outb(unsigned short port, unsigned char value)
{
    (void)port;
    (void)value;
}

/** Return 0xFF, as an absent port reads. */
unsigned char inb(unsigned short port)
{
    (void)port;
    return 0xFF;
}

/**
 * Take a halfword write at the data port the register's own offset selects,
 * 0xCFC or 0xCFE, which changes those two bytes and no others. The write is
 * recorded under its dword offset in the same record as a dword write, so a
 * row can ask which came first.
 */
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
        table_at[nwrites] = msix_table ? msix_table[0] : 0;
        nwrites++;
    }
}

/** Read the halfword at 0xCFC or 0xCFE, as `outw` writes it. */
unsigned short inw(unsigned short port)
{
    unsigned int off;

    if (port != 0xCFC && port != 0xCFE) {
        return 0xFFFF;
    }
    off = (address_port & 0xFC) + (unsigned int)(port - 0xCFC);
    return (unsigned short)((unsigned int)space[off] | ((unsigned int)space[off + 1] << 8));
}

/* The kernel takes each port access with interrupts off. On the host that is
 * a pair of counters, the depth of `kirq_save` calls not yet restored and the
 * restores that had no save, so the last row can require that every access
 * was balanced. */
static int irq_depth;
static int irq_unbalanced;

/** Count one level of interrupts off. */
unsigned int kirq_save(void)
{
    irq_depth++;
    return 0;
}

/** Undo one level of interrupts off, or count a restore with no save. */
void kirq_restore(unsigned int flags)
{
    (void)flags;
    if (irq_depth == 0) {
        irq_unbalanced++;
    } else {
        irq_depth--;
    }
}

/** Discard the message: no row checks the log. */
void klog(int level, const char *subsystem, const char *fmt, ...)
{
    (void)level;
    (void)subsystem;
    (void)fmt;
}

/* The local APIC identity of the boot processor, set by each row. */
static unsigned int the_apic_id;

/** Return the identity a row set in `the_apic_id`. */
unsigned int lapic_id(void)
{
    return the_apic_id;
}

/* ---- the rows ---- */

static int fails;

/** Count and print a failed row when `ok` is 0. */
static void check(int ok, const char *what)
{
    if (!ok) {
        fails++;
        printf("pcimsi_hosttest: FAIL: %s\n", what);
    }
}

/** Return the halfword at `off` in the model's configuration space. */
static unsigned short cfg16(unsigned char off)
{
    return (unsigned short)(space[off] | (space[off + 1] << 8));
}

/** Return the dword at `off` in the model's configuration space. */
static unsigned int cfg32(unsigned char off)
{
    return (unsigned int)space[off] | ((unsigned int)space[off + 1] << 8) |
           ((unsigned int)space[off + 2] << 16) | ((unsigned int)space[off + 3] << 24);
}

/**
 * Return the index in the write record of the first write to the dword that
 * holds `off`, or -1 when there is none.
 */
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

/**
 * Return the index of the first write to the dword that holds `off` whose
 * value has `bits` set in the halfword at `off`, or -1 when there is none.
 */
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

/**
 * Run the MSI-X rows on `dev`. The table is in the device's memory, not in
 * its configuration space, so the model's table is an array the rows read
 * after the call. The device comes in with MSI enabled by firmware and entry
 * 0 masked, as a device out of reset has it, with a stale address in it.
 * These are the last rows, so they end with the check that every access of
 * the run left the interrupt flag as it found it.
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
    msix_table = table;
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
        check(masked >= 0 && last > masked && table_at[masked] == 0xDEADBEEFu &&
                  table_at[last] == (PCI_MSI_ADDRESS_BASE | (2u << 12)),
              "the table is written after MSI-X is enabled masked and before it is unmasked");
    }
    msix_table = 0;

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

/**
 * Run the rows and print the result; return 1 when a row failed.
 *
 * The 32-bit capability comes in as firmware might leave it, enabled with
 * four of its eight vectors in use, and the first write must take both down,
 * since a device left enabled would send from the address it had. The rows
 * find the first write that sets the enable bit, since a later one would
 * hide an enable that came before the address. The 64-bit capability comes
 * in with a stale upper half, since a device programmed for a 64-bit
 * destination and given a 32-bit one would send to neither. A device left
 * with its own vector masked would be programmed, enabled and silent. A
 * capability near the end of configuration space would wrap its writes onto
 * the identity registers at zero, since an offset is one byte. A halfword
 * write must leave the status word beside the command word alone: its bits
 * are cleared by writing them as one.
 */
int main(void)
{
    pci_device_t dev;
    int rc;

    dev.bus = 0;
    dev.device = 4;
    dev.function = 0;
    the_apic_id = 0;

    /* A 32-bit capability: the address at 4 and the data at 8. */
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

    /* A 64-bit capability: the address's upper half at 8, the data at 12. */
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

    /* A device that can mask its own vectors, left with this one masked. */
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

    /* A capability whose registers would run past configuration space. */
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

    /* A halfword write to the command word, beside a latched status bit. */
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
