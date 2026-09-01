/*
 * pcimsi_hosttest.c: `pci_msi_enable` against a model of configuration space.
 *
 * host-deps: rach2/sys/pci.c
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

void outw(unsigned short port, unsigned short value)
{
    (void)port;
    (void)value;
}

unsigned short inw(unsigned short port)
{
    (void)port;
    return 0xFFFF;
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

int main(void)
{
    pci_device_t dev;
    int rc;

    dev.bus = 0;
    dev.device = 4;
    dev.function = 0;
    the_apic_id = 0;

    /* A 32-bit capability, which QEMU's HDA is not and the specification
       still allows: the address at 4 and the data at 8. */
    model_reset();
    model_cap(0x50, PCI_CAP_ID_MSI, 0x34);
    space[0x52] = 0x0E; /* four vectors offered, none used, 32-bit */
    rc = pci_msi_enable(&dev, 48);
    check(rc == 0, "a device with a 32-bit MSI capability is programmed");
    check(cfg32(0x54) == (PCI_MSI_ADDRESS_BASE | 0), "the address is the APIC's page");
    check(cfg16(0x58) == 48, "the data is the vector, at offset 8");
    check((cfg16(0x52) & PCI_MSI_ENABLE) != 0, "and the enable bit is set");
    check((cfg16(0x52) & PCI_MSI_MULTIPLE_ENABLE) == 0, "with one vector to use");
    check((cfg16(0x52) & 0x000Eu) == 0x000Eu, "and the count offered left alone");
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
        check(first_write_of(0x50) < address_at, "and the enable bit is cleared before either");
    }

    /* A 64-bit capability: the upper half zero and the data at 12. */
    model_reset();
    model_cap(0x60, PCI_CAP_ID_MSI, 0x34);
    space[0x62] = 0x80; /* one vector, 64-bit address */
    the_apic_id = 3;
    check(pci_msi_enable(&dev, 49) == 0, "a device with a 64-bit MSI capability is programmed");
    check(cfg32(0x64) == (PCI_MSI_ADDRESS_BASE | (3u << 12)),
          "the address carries the processor's identity");
    check(cfg32(0x68) == 0, "the upper half of the address is zero");
    check(cfg16(0x6C) == 49, "the data is the vector, at offset 12");
    check((cfg16(0x62) & PCI_MSI_ENABLE) != 0, "and the enable bit is set");

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
    check(pci_msi_enable(&dev, 51) == -1, "a device with no capability list is refused");
    check(nwrites == 0, "and nothing is written");

    model_reset();
    model_cap(0x90, PCI_CAP_ID_MSIX, 0x34);
    space[0x92] = 0x0F;
    check(pci_msi_enable(&dev, 52) == -1, "a device with MSI-X and no MSI is refused");
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

    if (fails == 0) {
        printf("pcimsi_hosttest: OK\n");
        return 0;
    }
    printf("pcimsi_hosttest: %d failure(s)\n", fails);
    return 1;
}
