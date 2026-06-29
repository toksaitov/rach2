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
#define AHCI_CAP  0x00 /* host capabilities */
#define AHCI_GHC  0x04 /* global host control */
#define AHCI_PI   0x0C /* ports implemented (bitmask) */
#define AHCI_VS   0x10 /* version */
#define AHCI_CAP2 0x24 /* extended capabilities */
#define AHCI_BOHC 0x28 /* BIOS/OS handoff control and status */

#define AHCI_GHC_AE 0x80000000u /* AHCI enable */
#define AHCI_GHC_IE 0x00000002u /* global interrupt enable */
#define AHCI_GHC_HR 0x00000001u /* HBA reset (self-clears when done) */
#define AHCI_CAP2_BOH 0x1u      /* BIOS/OS handoff supported */
#define AHCI_BOHC_BOS 0x1u      /* BIOS owned */
#define AHCI_BOHC_OOS 0x2u      /* OS ownership request */
#define AHCI_CAP_NP_MASK 0x1Fu  /* CAP.NP: number of ports minus one */

/* Per-port register block N lives at ABAR + 0x100 + N*0x80. */
#define AHCI_PORT_BASE   0x100
#define AHCI_PORT_STRIDE 0x80
#define AHCI_PxCLB  0x00 /* command list base, low 32 */
#define AHCI_PxCLBU 0x04 /* command list base, high 32 */
#define AHCI_PxFB   0x08 /* received-FIS base, low 32 */
#define AHCI_PxFBU  0x0C /* received-FIS base, high 32 */
#define AHCI_PxIS   0x10 /* interrupt status */
#define AHCI_PxIE   0x14 /* interrupt enable */
#define AHCI_PxCMD  0x18 /* command and status */
#define AHCI_PxTFD  0x20 /* task file data (STS/ERR) */
#define AHCI_PxSIG  0x24 /* device signature */
#define AHCI_PxSSTS 0x28 /* SATA status */
#define AHCI_PxSERR 0x30 /* SATA error (write 1 to clear) */
#define AHCI_PxSACT 0x34 /* SATA active */
#define AHCI_PxCI   0x38 /* command issue */

#define AHCI_PxCMD_ST  0x0001u /* start (command engine) */
#define AHCI_PxCMD_FRE 0x0010u /* FIS receive enable */
#define AHCI_PxCMD_FR  0x4000u /* FIS receive running */
#define AHCI_PxCMD_CR  0x8000u /* command list running */

/* SSTS: DET (bits 0-3) == 3 is device present + PHY ready; IPM (bits 8-11) == 1
   is the active power state. */
#define AHCI_DET_MASK    0xFu
#define AHCI_DET_PRESENT 0x3u
#define AHCI_IPM_ACTIVE  0x1u

/* Iteration watchdogs (no timer to bound by when the I/O path runs at IF=0).
   The link wait is shorter so an empty port skips quickly during bring-up. */
#define AHCI_SPIN_LIMIT 10000000u
#define AHCI_LINK_SPIN  1000000u

/* The DMA regions the HBA reads and writes. They are static so they land in
   .bss below HARD_MEMORY_LIMIT, where the identity map makes the virtual
   address equal the physical DMA address and the bytes are contiguous and
   below 4 GiB. The alignments are AHCI-mandated. The bring-up needs only the
   base addresses; a later phase lays typed records over the bytes. */
#define AHCI_CMDLIST_SIZE  1024 /* 32 command headers of 32 bytes */
#define AHCI_RECV_FIS_SIZE 256
#define AHCI_CMDTABLE_SIZE 256  /* 128-byte header plus a 16-byte PRDT entry */

static volatile uint8_t ahci_cmdlist[AHCI_CMDLIST_SIZE]
    __attribute__((aligned(1024)));
static volatile uint8_t ahci_recv_fis[AHCI_RECV_FIS_SIZE]
    __attribute__((aligned(256)));
static volatile uint8_t ahci_cmdtable[AHCI_CMDTABLE_SIZE]
    __attribute__((aligned(128)));

/* The mapped ABAR, NULL until ahci_init succeeds. MMIO, so volatile. The
   selected port (first implemented port with a device), -1 until bring-up. */
static pci_device_t ahci_pci;
static volatile uint8_t *ahci_base;
static int ahci_port = -1;

static uint32_t ahci_read32(uint32_t offset)
{
    return *(volatile uint32_t *)(ahci_base + offset);
}

static void ahci_write32(uint32_t offset, uint32_t value)
{
    *(volatile uint32_t *)(ahci_base + offset) = value;
}

/* The register offset of port `port`'s register `reg`. */
static uint32_t ahci_port_off(int port, uint32_t reg)
{
    return AHCI_PORT_BASE + (uint32_t) port * AHCI_PORT_STRIDE + reg;
}

/* Poll until every bit of `mask` at `offset` reads 0. Returns 0 on success, -1
   when the iteration watchdog expires (a wedged HBA). */
static int ahci_wait_clear(uint32_t offset, uint32_t mask)
{
    uint32_t spin = AHCI_SPIN_LIMIT;

    while (ahci_read32(offset) & mask) {
        if (--spin == 0) {
            return -1;
        }
    }
    return 0;
}

/* Find and map the AHCI HBA on the PCI bus. Discovery only, no transfer. */
static int ahci_discover(void)
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

    /* ABAR is BAR5. Reject any BAR4 64-bit spill or invalid BAR5 type. */
    if (pci_bar_is_64bit(dev.bar[4]) || pci_bar_is_64bit(dev.bar[5]) ||
        !pci_bar_is_memory(dev.bar[5])) {
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

    /* Decode MMIO now. Bus-master DMA is enabled only after the selected port's
       DMA bases are programmed. */
    ahci_pci = dev;
    pci_enable_memory_space(&ahci_pci);

    return 0;
}

/* Take ownership of the HBA and ensure AHCI mode.
 *
 * The HBA-wide reset (GHC.HR) is deliberately skipped. Without the full
 * per-port COMRESET handshake (deferred this milestone), an HR leaves the SATA
 * link needing re-establishment and the device not ready: on QEMU ich9-ahci,
 * the port's TFD then loses DRDY (0x50 becomes 0x130). The all-port quiesce
 * below gives owned ports without disturbing the link. A real-hardware
 * bring-up would HR and then COMRESET each port. */
static int ahci_hba_own(void)
{
    /* BIOS/OS handoff, only when the HBA advertises it (QEMU does not). */
    if (ahci_read32(AHCI_CAP2) & AHCI_CAP2_BOH) {
        ahci_write32(AHCI_BOHC, ahci_read32(AHCI_BOHC) | AHCI_BOHC_OOS);
        if (ahci_wait_clear(AHCI_BOHC, AHCI_BOHC_BOS) != 0) {
            return -1;
        }
    }

    ahci_write32(AHCI_GHC, (ahci_read32(AHCI_GHC) | AHCI_GHC_AE) &
                           ~AHCI_GHC_IE);
    return 0;
}

/* Stop one port and verify that no old command issue state remains. */
static int ahci_port_stop(int port)
{
    uint32_t cmd_off = ahci_port_off(port, AHCI_PxCMD);
    uint32_t cmd = ahci_read32(cmd_off);

    cmd &= ~AHCI_PxCMD_ST;
    ahci_write32(cmd_off, cmd);
    if (ahci_wait_clear(cmd_off, AHCI_PxCMD_CR) != 0) {
        return -1;
    }

    cmd = ahci_read32(cmd_off) & ~AHCI_PxCMD_FRE;
    ahci_write32(cmd_off, cmd);
    if (ahci_wait_clear(cmd_off, AHCI_PxCMD_FR) != 0) {
        return -1;
    }

    if (ahci_wait_clear(ahci_port_off(port, AHCI_PxCI), 0xFFFFFFFFu) != 0) {
        return -1;
    }
    if (ahci_wait_clear(ahci_port_off(port, AHCI_PxSACT), 0xFFFFFFFFu) != 0) {
        return -1;
    }
    return 0;
}

/* Mask AHCI interrupts and quiesce every implemented port before reuse. */
static int ahci_quiesce_ports(void)
{
    uint32_t pi = ahci_read32(AHCI_PI);
    int p;

    for (p = 0; p < 32; ++p) {
        if (!(pi & (1u << p))) {
            continue;
        }
        ahci_write32(ahci_port_off(p, AHCI_PxIE), 0);
        if (ahci_port_stop(p) != 0) {
            return -1;
        }
        ahci_write32(ahci_port_off(p, AHCI_PxSERR), 0xFFFFFFFFu);
        ahci_write32(ahci_port_off(p, AHCI_PxIS), 0xFFFFFFFFu);
    }
    return 0;
}

/* Select the first implemented port with a device present and the link up. */
static int ahci_select_port(void)
{
    uint32_t pi = ahci_read32(AHCI_PI);
    int p;

    for (p = 0; p < 32; ++p) {
        uint32_t ssts = 0;
        uint32_t spin = AHCI_LINK_SPIN;

        if (!(pi & (1u << p))) {
            continue;
        }
        do { /* let the link settle while scanning */
            ssts = ahci_read32(ahci_port_off(p, AHCI_PxSSTS));
            if ((ssts & AHCI_DET_MASK) == AHCI_DET_PRESENT) {
                break;
            }
        } while (--spin != 0);
        if ((ssts & AHCI_DET_MASK) == AHCI_DET_PRESENT &&
            ((ssts >> 8) & 0xF) == AHCI_IPM_ACTIVE) {
            ahci_port = p;
            return 0;
        }
    }
    return -1;
}

/* Stop the port engine, program the DMA bases, and restart it. */
static int ahci_port_start(void)
{
    uint32_t cmd_off = ahci_port_off(ahci_port, AHCI_PxCMD);

    if (ahci_port_stop(ahci_port) != 0) {
        return -1;
    }

    /* Program the command list and received-FIS bases (32-bit; upper = 0). */
    ahci_write32(ahci_port_off(ahci_port, AHCI_PxCLB),
                 (uint32_t) (uintptr_t) ahci_cmdlist);
    ahci_write32(ahci_port_off(ahci_port, AHCI_PxCLBU), 0);
    ahci_write32(ahci_port_off(ahci_port, AHCI_PxFB),
                 (uint32_t) (uintptr_t) ahci_recv_fis);
    ahci_write32(ahci_port_off(ahci_port, AHCI_PxFBU), 0);

    /* Clear latched errors and interrupt status (write 1 to clear). */
    ahci_write32(ahci_port_off(ahci_port, AHCI_PxSERR), 0xFFFFFFFFu);
    ahci_write32(ahci_port_off(ahci_port, AHCI_PxIS), 0xFFFFFFFFu);

    pci_enable_bus_master(&ahci_pci);

    /* Start: FRE first, then ST. */
    ahci_write32(cmd_off, ahci_read32(cmd_off) | AHCI_PxCMD_FRE);
    ahci_write32(cmd_off, ahci_read32(cmd_off) | AHCI_PxCMD_ST);
    return 0;
}

int ahci_init(void)
{
    ahci_base = 0;
    ahci_port = -1;
    if (ahci_discover() != 0) {
        return -1;
    }
    if (ahci_hba_own() != 0) {
        return -1;
    }
    if (ahci_quiesce_ports() != 0) {
        return -1;
    }
    if (ahci_select_port() != 0) {
        return -1;
    }
    if (ahci_port_start() != 0) {
        return -1;
    }
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
    serial_printf("ahci: CAP=%X GHC=%X PI=%X VS=%X ports=%d active=%d\n",
                  cap, ghc, pi, vs, (int) ((cap & AHCI_CAP_NP_MASK) + 1),
                  ahci_port);
    serial_printf("ahci: dma cmdlist=%X fis=%X table=%X\n",
                  (uint32_t) (uintptr_t) ahci_cmdlist,
                  (uint32_t) (uintptr_t) ahci_recv_fis,
                  (uint32_t) (uintptr_t) ahci_cmdtable);

    for (port = 0; port < 32; ++port) {
        uint32_t ssts, sig, tfd, cmd, clb;

        if (!(pi & (1u << port))) {
            continue;
        }
        ssts = ahci_read32(ahci_port_off(port, AHCI_PxSSTS));
        sig = ahci_read32(ahci_port_off(port, AHCI_PxSIG));
        tfd = ahci_read32(ahci_port_off(port, AHCI_PxTFD));
        cmd = ahci_read32(ahci_port_off(port, AHCI_PxCMD));
        clb = ahci_read32(ahci_port_off(port, AHCI_PxCLB));
        serial_printf("ahci:  port %d: SSTS=%X (DET=%d) SIG=%X TFD=%X CMD=%X CLB=%X\n",
                      port, ssts, (int) (ssts & AHCI_DET_MASK), sig, tfd, cmd, clb);
    }
}
