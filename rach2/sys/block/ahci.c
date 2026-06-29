#include "ahci.h"

#include "../pci.h"
#include "../serial.h"
#include "../mm/paging.h"

#include "../lib/kint.h"

/* AHCI prog-if within PCI class 0x01 subclass 0x06 (PCI_CLASS_MASS_STORAGE /
   PCI_SUBCLASS_SATA). A controller in IDE-compat or RAID mode reports a
   different prog-if and is skipped. */
#define AHCI_PCI_PROGIF 0x01

/* The ABAR (BAR5) MMIO span: the generic host control block (~0x100 bytes) plus
   0x80 per port for up to 32 ports; 0x2000 over-covers it. */
#define AHCI_ABAR_MAP_SIZE 0x2000

/* Generic host control registers, as offsets from the ABAR. */
#define AHCI_CAP 0x00 /* host capabilities */
#define AHCI_GHC 0x04 /* global host control */
#define AHCI_PI  0x0C /* ports implemented (bitmask) */
#define AHCI_VS  0x10 /* version */

/* CAP.NP: number of ports minus one, in bits 0-4. */
#define AHCI_CAP_NP_MASK 0x1Fu

/* Per-port register block N lives at ABAR + 0x100 + N*0x80. */
#define AHCI_PORT_BASE   0x100
#define AHCI_PORT_STRIDE 0x80
#define AHCI_PxCMD  0x18 /* command and status */
#define AHCI_PxTFD  0x20 /* task file data (STS/ERR) */
#define AHCI_PxSIG  0x24 /* device signature */
#define AHCI_PxSSTS 0x28 /* SATA status (DET in bits 0-3) */

/* SSTS.DET == 3: device present and the PHY communication is established. */
#define AHCI_DET_MASK 0xFu

/* The mapped ABAR, NULL until ahci_init succeeds. MMIO, so volatile. */
static volatile uint8_t *ahci_base;

static uint32_t ahci_read32(uint32_t offset)
{
    return *(volatile uint32_t *)(ahci_base + offset);
}

/* The register offset of port `port`'s register `reg`. */
static uint32_t ahci_port_off(int port, uint32_t reg)
{
    return AHCI_PORT_BASE + (uint32_t) port * AHCI_PORT_STRIDE + reg;
}

int ahci_init(void)
{
    pci_device_t dev;
    pci_device_t skip;
    pci_device_t *skip_dev = 0;
    uint32_t abar;
    int found = 0;

    while (pci_find_device_skip(PCI_CLASS_MASS_STORAGE, PCI_SUBCLASS_SATA,
                                skip_dev, &dev)) {
        if (dev.prog_if == AHCI_PCI_PROGIF) {
            found = 1;
            break;
        }
        skip = dev; /* persistent storage for the next skip */
        skip_dev = &skip;
    }
    if (!found) {
        return -1;
    }

    /* ABAR is BAR5, a memory BAR; guard against a 64-bit BAR4 spilling into it. */
    if (!pci_bar_is_memory(dev.bar[5])) {
        return -1;
    }
    abar = pci_bar_address(dev.bar[5]);
    if (abar == 0) {
        return -1;
    }
    if (abar >= HARD_MEMORY_LIMIT) {
        if (map_mmio(abar, AHCI_ABAR_MAP_SIZE) == 0) {
            return -1;
        }
    }
    ahci_base = (volatile uint8_t *) (uintptr_t) abar;

    /* MMIO decoding and bus-master DMA must both be on before any HBA transfer,
       even in polled mode (the HBA masters the bus for the DMA itself). */
    pci_enable_memory_space(&dev);
    pci_enable_bus_master(&dev);

    return 0;
}

void ahci_dump(void)
{
    uint32_t cap, ghc, pi, vs;
    int port;

    if (!ahci_base) {
        serial_printf("ahci: not initialized\n");
        return;
    }

    cap = ahci_read32(AHCI_CAP);
    ghc = ahci_read32(AHCI_GHC);
    pi = ahci_read32(AHCI_PI);
    vs = ahci_read32(AHCI_VS);
    serial_printf("ahci: CAP=%X GHC=%X PI=%X VS=%X ports=%d\n",
                  cap, ghc, pi, vs, (int) ((cap & AHCI_CAP_NP_MASK) + 1));

    for (port = 0; port < 32; ++port) {
        uint32_t ssts, sig, tfd, cmd;

        if (!(pi & (1u << port))) {
            continue;
        }
        ssts = ahci_read32(ahci_port_off(port, AHCI_PxSSTS));
        sig = ahci_read32(ahci_port_off(port, AHCI_PxSIG));
        tfd = ahci_read32(ahci_port_off(port, AHCI_PxTFD));
        cmd = ahci_read32(ahci_port_off(port, AHCI_PxCMD));
        serial_printf("ahci:  port %d: SSTS=%X (DET=%d) SIG=%X TFD=%X CMD=%X\n",
                      port, ssts, (int) (ssts & AHCI_DET_MASK), sig, tfd, cmd);
    }
}
