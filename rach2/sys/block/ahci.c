#include "ahci.h"
#include "diskcmd.h"
#include "diskinfo.h"
#include "../udelay.h"
#include "block.h"

#include "../pci.h"
#include "../serial.h"
#include "../mm/paging.h"

#include "../lib/kerrno.h"
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

#define AHCI_GHC_AE      0x80000000u /* AHCI enable */
#define AHCI_GHC_IE      0x00000002u /* global interrupt enable */
#define AHCI_CAP2_BOH    0x1u        /* BIOS/OS handoff supported */
#define AHCI_BOHC_BOS    0x1u        /* BIOS owned */
#define AHCI_BOHC_OOS    0x2u        /* OS ownership request */
#define AHCI_CAP_NP_MASK 0x1Fu       /* CAP.NP: number of ports minus one */

/* Per-port register block N lives at ABAR + 0x100 + N*0x80. */
#define AHCI_PORT_BASE   0x100
#define AHCI_PORT_STRIDE 0x80
#define AHCI_PxCLB       0x00 /* command list base, low 32 */
#define AHCI_PxCLBU      0x04 /* command list base, high 32 */
#define AHCI_PxFB        0x08 /* received-FIS base, low 32 */
#define AHCI_PxFBU       0x0C /* received-FIS base, high 32 */
#define AHCI_PxIS        0x10 /* interrupt status */
#define AHCI_PxIE        0x14 /* interrupt enable */
#define AHCI_PxCMD       0x18 /* command and status */
#define AHCI_PxTFD       0x20 /* task file data (STS/ERR) */
#define AHCI_PxSIG       0x24 /* device signature */
#define AHCI_PxSSTS      0x28 /* SATA status */
#define AHCI_PxSCTL      0x2C /* SATA control: DET in bits 0-3 */
#define AHCI_PxSERR      0x30 /* SATA error (write 1 to clear) */
#define AHCI_PxSACT      0x34 /* SATA active */
#define AHCI_PxCI        0x38 /* command issue */

#define AHCI_PxCMD_ST  0x0001u /* start (command engine) */
#define AHCI_PxCMD_SUD 0x0002u /* spin-up device (staggered spin-up) */
#define AHCI_PxCMD_POD 0x0004u /* power on device (cold presence detect) */
#define AHCI_PxCMD_FRE 0x0010u /* FIS receive enable */
#define AHCI_PxCMD_FR  0x4000u /* FIS receive running */
#define AHCI_PxCMD_CR  0x8000u /* command list running */

/* PxCMD.ICC, the interface communication control: writing 1 asks the link to
   go active; the HBA clears the field when the transition is done. */
#define AHCI_PxCMD_ICC_MASK   0xF0000000u
#define AHCI_PxCMD_ICC_ACTIVE 0x10000000u

/* PCS, the Intel port control and status word in PCI CONFIG space: one enable
   bit per port. Not an AHCI register at all, which is why it is easy to miss
   and why Linux carries a named quirk for it. */
#define AHCI_INTEL_PCS 0x92

/* SSTS: DET (bits 0-3) == 3 is device present + PHY ready; IPM (bits 8-11) == 1
   is the active power state. */
#define AHCI_DET_MASK    0xFu
#define AHCI_DET_PRESENT 0x3u
#define AHCI_IPM_ACTIVE  0x1u

/* SCTL.DET: 1 requests a COMRESET, 0 lets the link come up again. */
#define AHCI_SCTL_DET_MASK 0xFu
#define AHCI_SCTL_DET_INIT 0x1u
#define AHCI_SCTL_DET_NONE 0x0u

/* SCTL.IPM with every link power state forbidden: partial, slumber and
   DevSleep. Forbidding them host-side also makes the HBA refuse the DEVICE'S
   requests to enter them, so one write covers both halves of what Windows
   calls HIPM and DIPM. */
#define AHCI_SCTL_IPM_MASK        0xF00u
#define AHCI_SCTL_IPM_DISABLE_ALL 0x700u

/* GHC.HR: the HBA-wide reset, which the HBA clears when it is done. */
#define AHCI_GHC_HR 0x00000001u

/* PxSIG, the signature the device leaves after a reset. A plain SATA disk
 * answers 0x00000101; an ATAPI device answers 0xEB140101, a port multiplier
 * 0x96690101, and an enclosure service 0xC33C0101. This driver drives disks. */
#define AHCI_SIG_ATA   0x00000101u
#define AHCI_SIG_ATAPI 0xEB140101u

/* Every wait is measured against the timer chip rather than counted, because
   an iteration count is really a bet on the processor's speed: ten million raw
   register reads are a moment on the QEMU loop and several seconds on the
   laptop this drives. The budgets: one second for a handshake (the host reset
   is allowed exactly that by the specification, and the rest are shorter in
   practice), and one second at a fine grain for a command, which a healthy
   device answers in microseconds. */
#define AHCI_WAIT_POLL_US 100u
#define AHCI_WAIT_STEPS   10000u
#define AHCI_CMD_POLL_US  10u
#define AHCI_CMD_STEPS    100000u

/* The two waits that follow a COMRESET are measured rather than counted, since
   what they wait for happens on the wire and in the device rather than at the
   processor's speed. The poll interval is 100 us; the budgets are one second
   for the PHY to negotiate, which is what the specification allows, and five
   for the device to finish its own reset and post its signature, which is
   generous for a disk and short enough that a port with something unresponsive
   on it does not hold up a boot. Both expiries are ordinary answers: an empty
   port and a device that will not answer are both ports to move past. */
#define AHCI_RESET_POLL_US 100u
#define AHCI_LINK_STEPS    10000u
#define AHCI_READY_STEPS   50000u

/* The DMA regions the HBA reads and writes. They are static so they land in
   .bss below HARD_MEMORY_LIMIT, where the identity map makes the virtual
   address equal the physical DMA address and the bytes are contiguous and
   below 4 GiB. The alignments are AHCI-mandated. The bring-up needs only the
   base addresses; a later phase lays typed records over the bytes. */
#define AHCI_CMDLIST_SIZE  1024 /* 32 command headers of 32 bytes */
#define AHCI_RECV_FIS_SIZE 256
#define AHCI_CMDTABLE_SIZE 256 /* 128-byte header plus a 16-byte PRDT entry */

static volatile uint8_t ahci_cmdlist[AHCI_CMDLIST_SIZE] __attribute__((aligned(1024)));
static volatile uint8_t ahci_recv_fis[AHCI_RECV_FIS_SIZE] __attribute__((aligned(256)));
static volatile uint8_t ahci_cmdtable[AHCI_CMDTABLE_SIZE] __attribute__((aligned(128)));

/* Task-file status bits (PxTFD low byte) and the fatal port-interrupt bits. A
   command fails on a device error (PxTFD ERR or DF) or on a fatal PxIS bit (the
   task-file error or a host-bus or interface fault PxTFD never reflects). */
#define AHCI_PxTFD_BSY  0x80u
#define AHCI_PxTFD_DRQ  0x08u
#define AHCI_PxTFD_DF   0x20u /* device fault, independent of ERR */
#define AHCI_PxTFD_ERR  0x01u
#define AHCI_PxIS_TFES  0x40000000u /* task file error status */
#define AHCI_PxIS_HBFS  0x20000000u /* host bus fatal error */
#define AHCI_PxIS_HBDS  0x10000000u /* host bus data error */
#define AHCI_PxIS_IFS   0x08000000u /* interface fatal error */
#define AHCI_PxIS_FATAL (AHCI_PxIS_TFES | AHCI_PxIS_HBFS | AHCI_PxIS_HBDS | AHCI_PxIS_IFS)

/* The H2D Register FIS and the ATA commands the polled path issues. */
#define AHCI_FIS_H2D      0x27u
#define AHCI_FIS_H2D_C    0x80u /* C bit: this FIS carries a command */
#define ATA_READ_DMA_EXT  0x25u
#define ATA_WRITE_DMA_EXT 0x35u
#define ATA_FLUSH_EXT     0xEAu /* FLUSH CACHE EXT, durability after a write */
#define ATA_IDENTIFY      0xECu
#define ATA_DEV_LBA       0x40u /* device register: LBA mode */

/* One PRDT entry transfers up to 4 MiB, so cap a single command there; every
   exercised caller (the page cache, 8 sectors) is far under it, and a larger
   request fails closed rather than silently truncating the byte count. */
#define AHCI_MAX_SECTORS_PER_CMD 8192u

/* The single polled command slot, its 5-dword H2D FIS, and the PRDT offset
   (after CFIS 64 + ACMD 16 + reserved 48) inside the command table. */
#define AHCI_SLOT        0
#define AHCI_CFL_DWORDS  5
#define AHCI_CMD_W       (1u << 6)  /* command header: write direction */
#define AHCI_PRDTL_ONE   (1u << 16) /* command header: one PRDT entry */
#define AHCI_PRDT_OFFSET 0x80
#define AHCI_DBC_MASK    0x3FFFFFu /* PRDT byte count, bits 0-21 */

/* Typed overlays on the DMA byte regions: one command-list header (the HBA
   reads 32 bytes per slot, of which only the first 16 are software-set; the
   rest stay zero), the H2D FIS at the command-table head, and one PRDT
   entry. */
typedef struct {
    uint32_t dw0;   /* CFL[4:0], W[6], PRDTL[31:16] */
    uint32_t prdbc; /* byte count the HBA writes back */
    uint32_t ctba;  /* command table base, low 32 */
    uint32_t ctbau; /* command table base, high 32 */
} __attribute__((packed)) ahci_cmd_header_t;

typedef struct {
    uint8_t fis_type; /* 0x27 */
    uint8_t pmport_c; /* C bit and port-multiplier port */
    uint8_t command;
    uint8_t featurel;
    uint8_t lba0, lba1, lba2; /* LBA 7:0, 15:8, 23:16 */
    uint8_t device;           /* 0x40 for LBA mode */
    uint8_t lba3, lba4, lba5; /* LBA 31:24, 39:32, 47:40 */
    uint8_t featureh;
    uint8_t countl, counth;
    uint8_t icc;
    uint8_t control;
    uint32_t aux; /* reserved */
} __attribute__((packed)) ahci_h2d_fis_t;

typedef struct {
    uint32_t dba;  /* data base, low 32 */
    uint32_t dbau; /* data base, high 32 */
    uint32_t reserved;
    uint32_t dbc; /* byte count minus one [21:0], interrupt bit [31] */
} __attribute__((packed)) ahci_prdt_t;

/* The logical sector count from IDENTIFY words 100-103, 0 until known. */
static uint64_t ahci_sector_count;

/* The mapped ABAR, NULL until ahci_init succeeds. MMIO, so volatile. The
   selected port (first implemented port with a device), -1 until bring-up. */
static pci_device_t ahci_pci;
static volatile uint8_t *ahci_base;
static int ahci_port = -1;

/** Read the 32-bit ABAR register at `offset`. */
static uint32_t ahci_read32(uint32_t offset)
{
    return *(volatile uint32_t *)(ahci_base + offset);
}

/** Write `value` to the 32-bit ABAR register at `offset`. */
static void ahci_write32(uint32_t offset, uint32_t value)
{
    *(volatile uint32_t *)(ahci_base + offset) = value;
}

/** The register offset of port `port`'s register `reg`. */
static uint32_t ahci_port_off(int port, uint32_t reg)
{
    return AHCI_PORT_BASE + (uint32_t)port * AHCI_PORT_STRIDE + reg;
}

/* Cache/DMA coherency, a real-hardware requirement, mirroring the HDA driver's
   helper. x86 bus-master DMA snoops the caches, so this is a no-op in practice and a
   plain no-op on emulators (which report no CLFLUSH). */
static bool ahci_have_clflush = false;
static uint32_t ahci_clflush_size = 64;

/** Record whether CLFLUSH is available and its line size (CPUID leaf 1). */
static void ahci_detect_cache(void)
{
    uint32_t eax, ebx, ecx, edx;
    uint32_t sz;

    eax = 1;
    __asm__ __volatile__("cpuid" : "+a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : : "memory");

    ahci_have_clflush = ((edx & (1u << 19)) != 0);
    sz = ((ebx >> 8) & 0xFFu) * 8u;
    if (sz == 0) {
        sz = 64;
    }
    ahci_clflush_size = sz;
}

/** Evict [addr, addr+size) so a DMA write the CPU is about to read, or a
   descriptor the HBA is about to read, crosses the cache a line at a time. */
static void ahci_dma_sync(const volatile void *addr, uint32_t size)
{
    uintptr_t p, end, line;

    if (!addr || size == 0) {
        return;
    }
    if (ahci_have_clflush && ahci_clflush_size != 0) {
        line = (uintptr_t)ahci_clflush_size;
        p = (uintptr_t)addr & ~(line - 1u);
        end = (uintptr_t)addr + (uintptr_t)size;
        while (p < end) {
            /* clflush (%eax) as bytes 0f ae 38: tcc has no mnemonic for it. */
            __asm__ __volatile__(".byte 0x0f, 0xae, 0x38" ::"a"(p) : "memory");
            p += line;
        }
        __asm__ __volatile__("lock; addl $0, 0(%%esp)" ::: "memory");
    } else {
        __asm__ __volatile__("wbinvd" ::: "memory");
    }
}

/** Poll until every bit of `mask` at `offset` reads 0. Returns 0 on success, -1
   when the iteration watchdog expires (a wedged HBA). */
static int ahci_wait_clear(uint32_t offset, uint32_t mask)
{
    uint32_t spin;

    for (spin = 0; spin < AHCI_WAIT_STEPS; ++spin) {
        if ((ahci_read32(offset) & mask) == 0) {
            return 0;
        }
        k_udelay(AHCI_WAIT_POLL_US);
    }
    return -1;
}

/** Find and map the AHCI HBA on the PCI bus. Discovery only, no transfer. */
/* Why the last bring-up stopped. A silent -1 is no use on a machine whose
   only account of itself is what it printed, so every failure below names
   itself and ahci_init prints it. */
static const char *ahci_err = "no SATA controller in AHCI mode on the bus";

static int ahci_discover(void)
{
    pci_device_t dev;
    pci_device_t skip;
    pci_device_t *skip_dev = 0;
    uint32_t abar;
    int found = 0;

    while (pci_find_device_skip(PCI_CLASS_MASS_STORAGE, PCI_SUBCLASS_SATA, skip_dev, &dev)) {
        if (dev.prog_if == AHCI_PCI_PROGIF) {
            found = 1;
            break;
        }
        skip = dev;
        skip_dev = &skip;
    }
    if (!found) {
        /* Say what IS at the address a PCH keeps its SATA controller, because
         * "nothing matched" cannot tell an absent controller from a scan that
         * never visited it: the controller lives at function 2 of a
         * multi-function device, and reaching it depends on function 0
         * advertising its siblings. One direct read settles that in a line. */
        serial_printf("ahci: no match; 0:31.2 reads %X:%X class %X:%X prog-if %X\n",
                      (unsigned)pci_read_config16(0, 31, 2, PCI_VENDOR_ID),
                      (unsigned)pci_read_config16(0, 31, 2, PCI_DEVICE_ID),
                      (unsigned)pci_read_config8(0, 31, 2, PCI_CLASS),
                      (unsigned)pci_read_config8(0, 31, 2, PCI_SUBCLASS),
                      (unsigned)pci_read_config8(0, 31, 2, PCI_PROG_IF));
        ahci_err = "no SATA controller in AHCI mode on the bus";
        return -1;
    }

    serial_printf("ahci: %X:%X at %d:%d.%d, prog-if %X, BAR5 %X\n", (unsigned)dev.vendor_id,
                  (unsigned)dev.device_id, (int)dev.bus, (int)dev.device, (int)dev.function,
                  (unsigned)dev.prog_if, (unsigned)dev.bar[5]);

    /* ABAR is BAR5. Reject any BAR4 64-bit spill or invalid BAR5 type. */
    if (pci_bar_is_64bit(dev.bar[4]) || pci_bar_is_64bit(dev.bar[5]) ||
        !pci_bar_is_memory(dev.bar[5])) {
        ahci_err = "the controller's ABAR is not a plain 32-bit memory window";
        return -1;
    }
    abar = pci_bar_address(dev.bar[5]);
    if (abar == 0) {
        ahci_err = "the controller's ABAR was never programmed";
        return -1;
    }
    /* Map the ABAR cache-disabled (PCD|PWT) whatever its address. A low ABAR is
       already identity-mapped but cacheable, so a status poll could read stale;
       map_mmio rewrites the PTE cache-disabled either way. */
    if (map_mmio(abar, AHCI_ABAR_MAP_SIZE) == 0) {
        ahci_err = "the ABAR window could not be mapped";
        return -1;
    }
    ahci_base = (volatile uint8_t *)(uintptr_t)abar;

    /* Decode MMIO now. Bus-master DMA is enabled only after the selected port's
       DMA bases are programmed. */
    ahci_pci = dev;
    pci_enable_memory_space(&ahci_pci);

    return 0;
}

/**
 * Re-establish the SATA link on one port (COMRESET), or report that it is
 * empty. Returns 0 when a device is present and the PHY is ready.
 *
 * This is the recovery the laptop needs and the reason the HBA reset above can
 * be issued at all. A warm reboot on that machine leaves its SATA link in a
 * state where the port reports a device and the device never becomes ready,
 * and the only way back is to take the link down and bring it up: DET=1 holds
 * COMRESET asserted, and the specification requires it held for at least one
 * millisecond, after which DET=0 releases it and the PHY negotiates again.
 *
 * An empty port is not a failure. Every implemented port is reset at bring-up,
 * and most machines have ports with nothing on them; this reports that plainly
 * so the scan can move on.
 */
static int ahci_port_comreset(int port)
{
    uint32_t sctl_off = ahci_port_off(port, AHCI_PxSCTL);
    uint32_t ssts_off = ahci_port_off(port, AHCI_PxSSTS);
    uint32_t serr_off = ahci_port_off(port, AHCI_PxSERR);
    uint32_t cmd_off = ahci_port_off(port, AHCI_PxCMD);
    uint32_t sctl = ahci_read32(sctl_off);
    uint32_t spin;

    /* The device answers a reset with a FIS carrying its signature, and a FIS
     * arrives only if the port has somewhere to put it and is listening. So
     * the received-FIS area is programmed and FRE turned on before the reset
     * rather than in the start path afterwards; without this the task file
     * never updates and every port looks like a device that will not answer.
     * The command engine stays off: nothing is being issued here. */
    ahci_write32(ahci_port_off(port, AHCI_PxCLB), (uint32_t)(uintptr_t)ahci_cmdlist);
    ahci_write32(ahci_port_off(port, AHCI_PxCLBU), 0);
    ahci_write32(ahci_port_off(port, AHCI_PxFB), (uint32_t)(uintptr_t)ahci_recv_fis);
    ahci_write32(ahci_port_off(port, AHCI_PxFBU), 0);
    pci_enable_bus_master(&ahci_pci);

    /* Bring the port's power state up before asking it for a link. Our own
     * HBA reset put every port at its reset defaults, and on silicon that
     * supports staggered spin-up the default is spun DOWN: PxCMD.SUD resets
     * to 0 and the specification requires it be 1 for the COMRESET below to
     * act at all (AHCI 1.3.1, 10.4.2). QEMU could never show this, because
     * its model resets ports with spin-up and power already on, so every
     * suite passed while the real chip sat dark. Where the features are
     * absent both bits read as 1 and the writes change nothing, which is why
     * Linux (ahci_power_up) writes them unconditionally too; the ICC write
     * then asks the interface itself to go active. */
    {
        uint32_t cmd = ahci_read32(cmd_off) | AHCI_PxCMD_SUD | AHCI_PxCMD_POD;

        ahci_write32(cmd_off, cmd);
        ahci_write32(cmd_off, (cmd & ~AHCI_PxCMD_ICC_MASK) | AHCI_PxCMD_ICC_ACTIVE);
    }

    ahci_write32(cmd_off, ahci_read32(cmd_off) | AHCI_PxCMD_FRE);

    /* Link power management is forbidden outright, in the same write that
     * drives the reset and permanently thereafter. This machine's own history
     * is the reason: with its previous operating system, a warm reboot left
     * this drive invisible even to the firmware until power was removed, and
     * disabling the link power states in the storage driver cured it. Linux
     * documents the same temperament in this drive family (its quirk table
     * marks "Samsung SSD 870*", scoped there to ATI controllers), and its
     * strongest policy, ATA_LPM_MAX_POWER, is exactly this value: SControl
     * IPM = 7, every transition disabled (libata-sata.c). A diagnostic that
     * reads a sector an hour after boot must not find that the link slept in
     * between and the drive never woke. */
    sctl = (sctl & ~(AHCI_SCTL_DET_MASK | AHCI_SCTL_IPM_MASK)) | AHCI_SCTL_IPM_DISABLE_ALL;
    ahci_write32(sctl_off, sctl | AHCI_SCTL_DET_INIT);
    /* At least 1 ms with COMRESET asserted. */
    k_udelay(2000);
    ahci_write32(sctl_off, sctl | AHCI_SCTL_DET_NONE);

    /* The link then has to negotiate. A port with nothing on it never reaches
     * DET=3, so this is bounded and its expiry is an ordinary answer. */
    for (spin = 0; spin < AHCI_LINK_STEPS; ++spin) {
        if ((ahci_read32(ssts_off) & AHCI_DET_MASK) == AHCI_DET_PRESENT) {
            /* The errors a link-up necessarily raises are ours to clear, and
             * clearing them is what leaves the port usable. PxSERR is
             * write-1-to-clear, so writing back what was read clears exactly
             * the bits that were set. */
            ahci_write32(serr_off, ahci_read32(serr_off));
            return 0;
        }
        k_udelay(AHCI_RESET_POLL_US);
    }
    return -1;
}

/** Take ownership of the HBA and ensure AHCI mode.
 *
 * The HBA-wide reset (GHC.HR) is issued here, which it was not before: the
 * driver skipped it while there was no COMRESET, because an HR leaves every
 * SATA link needing re-establishment and a port whose link never comes back
 * reports a device that is not ready (on QEMU's ich9-ahci the port's TFD loses
 * DRDY, 0x50 becoming 0x130). With the per-port COMRESET below, that is a
 * state the driver can get out of, and a reset is worth having: the machine
 * this is being carried to hands the HBA over from firmware that has been
 * using it, and the only way to know what state the registers are in is to put
 * them in a known one.
 */
static int ahci_hba_own(void)
{
    /* BIOS/OS handoff, only when the HBA advertises it (QEMU does not). The
     * request is made and the answer waited for, but an unanswered request is
     * a warning rather than a refusal: Linux never performs this handoff at
     * all (BOHC appears in its AHCI driver only as a capability bit's name),
     * so firmware with a broken handoff semaphore would fail only us, on a
     * controller Linux would simply take. Asking first is still the polite
     * order; being refused by silence is not a reason to stop. */
    if (ahci_read32(AHCI_CAP2) & AHCI_CAP2_BOH) {
        ahci_write32(AHCI_BOHC, ahci_read32(AHCI_BOHC) | AHCI_BOHC_OOS);
        if (ahci_wait_clear(AHCI_BOHC, AHCI_BOHC_BOS) != 0) {
            serial_printf("ahci: the firmware did not answer the handoff; continuing\n");
        }
    }

    /* AHCI mode before the reset, as the specification requires: the HBA's
     * registers are only defined in AHCI mode, and that includes GHC.HR. */
    ahci_write32(AHCI_GHC, ahci_read32(AHCI_GHC) | AHCI_GHC_AE);
    ahci_write32(AHCI_GHC, ahci_read32(AHCI_GHC) | AHCI_GHC_HR);
    /* The HBA clears HR when it has finished; the specification allows it one
     * second, and a bit that never clears is an HBA this driver cannot use. */
    if (ahci_wait_clear(AHCI_GHC, AHCI_GHC_HR) != 0) {
        ahci_err = "the host reset never completed";
        return -1;
    }
    /* The reset clears AE, so AHCI mode is set again, and interrupts stay off:
     * this driver polls. */
    ahci_write32(AHCI_GHC, (ahci_read32(AHCI_GHC) | AHCI_GHC_AE) & ~AHCI_GHC_IE);

    /* Re-enable the port PHYs the reset may have left disabled. On Intel
     * controllers of this era a per-port enable lives in PCI configuration
     * space (PCS, word 0x92), outside everything GHC.HR touches or the AHCI
     * specification names; a port whose PCS bit is clear has no PHY, and no
     * COMRESET can wake it. Linux ORs the implemented-port map into PCS after
     * every controller reset and applies that quirk to this laptop's exact
     * controller by device id (ahci_intel_pcs_quirk, 8086:9c03 in its PCI
     * table). Gated to the Lynx Point and Wildcat Point families this
     * project's board profiles cover, because on newer Intel platforms the
     * register moved and a blind write here would poke something else. */
    if (ahci_pci.vendor_id == 0x8086 &&
        ((ahci_pci.device_id & 0xFF00u) == 0x8C00u || (ahci_pci.device_id & 0xFF00u) == 0x9C00u)) {
        uint16_t pcs =
            pci_read_config16(ahci_pci.bus, ahci_pci.device, ahci_pci.function, AHCI_INTEL_PCS);
        uint16_t ports = (uint16_t)(ahci_read32(AHCI_PI) & 0xFFFFu);

        if ((pcs & ports) != ports) {
            pci_write_config16(ahci_pci.bus, ahci_pci.device, ahci_pci.function, AHCI_INTEL_PCS,
                               (uint16_t)(pcs | ports));
            serial_printf("ahci: enabled ports %X in PCS (was %X)\n", (unsigned)ports,
                          (unsigned)pcs);
        }
    }
    return 0;
}

/** Stop one port and verify that no old command issue state remains. */
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

/** Mask AHCI interrupts and quiesce every implemented port before reuse. */
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
            ahci_err = "a port would not stop";
            return -1;
        }
        ahci_write32(ahci_port_off(p, AHCI_PxSERR), 0xFFFFFFFFu);
        ahci_write32(ahci_port_off(p, AHCI_PxIS), 0xFFFFFFFFu);
    }
    return 0;
}

/**
 * Wait for a port's device to finish its reset. Returns 0 when it has.
 *
 * After a COMRESET the device runs its own reset and then posts a D2H FIS
 * carrying its signature; until that has happened the task file reports busy
 * and the signature register holds nothing worth reading. Bounded, because a
 * device that never becomes ready is a device this driver must move past
 * rather than wait on.
 */
static int ahci_port_ready(int port)
{
    uint32_t tfd_off = ahci_port_off(port, AHCI_PxTFD);
    uint32_t spin;

    for (spin = 0; spin < AHCI_READY_STEPS; ++spin) {
        if (!(ahci_read32(tfd_off) & (AHCI_PxTFD_BSY | AHCI_PxTFD_DRQ))) {
            return 0;
        }
        k_udelay(AHCI_RESET_POLL_US);
    }
    return -1;
}

/**
 * Select the first implemented port carrying a SATA disk.
 *
 * Every implemented port is COMRESET first, because a link that a warm reboot
 * left wedged reports a device that never becomes ready, and the scan would
 * otherwise select it and then fail on the identify. A port that comes back
 * empty is skipped without comment, since most machines have some.
 *
 * The signature decides what the port carries, and only a disk is taken. An
 * optical drive answers the ATAPI signature and would fail every command this
 * driver knows; a port multiplier and an enclosure service answer their own.
 * Naming the one signature this driver drives, rather than excluding the ones
 * it happens to know about, means an unfamiliar device is skipped rather than
 * driven as if it were a disk.
 *
 * A port that is skipped is stopped again before the scan moves on. The reset
 * had to enable that port's FIS receive engine, and every port is pointed at
 * the same single received-FIS area, so a skipped port left running would go
 * on writing into the buffer the selected port is using. On the machine this
 * targets that is not hypothetical: it has a SATA disk and a bay that may hold
 * an optical drive, which is exactly two ports.
 */
static int ahci_select_port(void)
{
    uint32_t pi = ahci_read32(AHCI_PI);
    int p;

    for (p = 0; p < 32; ++p) {
        uint32_t ssts, sig;

        if (!(pi & (1u << p))) {
            continue;
        }
        if (ahci_port_comreset(p) != 0) {
            (void)ahci_port_stop(p); /* nothing on this port */
            continue;
        }
        ssts = ahci_read32(ahci_port_off(p, AHCI_PxSSTS));
        /* DET alone decides, as Linux's link-online test does
         * (ata_phys_link_online checks DET==3 and nothing else): a link that
         * negotiated and then dropped into a low-power interface state is a
         * live link, and the ICC write in the reset path asks it back up.
         * Requiring IPM to read active as well, which this did, silently
         * discards exactly that disk. */
        if ((ssts & AHCI_DET_MASK) != AHCI_DET_PRESENT) {
            (void)ahci_port_stop(p);
            continue;
        }
        /* The signature is only valid once the device has finished its own
         * reset and posted its first FIS; read before that it is all ones,
         * which is not a signature but a port that has not answered yet. */
        if (ahci_port_ready(p) != 0) {
            serial_printf("ahci: port %d: the device never became ready; skipped\n", p);
            (void)ahci_port_stop(p);
            continue;
        }
        sig = ahci_read32(ahci_port_off(p, AHCI_PxSIG));
        if (sig != AHCI_SIG_ATA) {
            serial_printf("ahci: port %d: signature %X is not a disk; skipped\n", p, sig);
            (void)ahci_port_stop(p);
            continue;
        }
        ahci_port = p;
        return 0;
    }
    ahci_err = "no port carries a disk";
    return -1;
}

/** Stop the port engine, program the DMA bases, and restart it. */
static int ahci_port_start(void)
{
    uint32_t cmd_off = ahci_port_off(ahci_port, AHCI_PxCMD);

    if (ahci_port_stop(ahci_port) != 0) {
        ahci_err = "the selected port would not stop";
        return -1;
    }

    /* Program the command list and received-FIS bases (32-bit; upper = 0). */
    ahci_write32(ahci_port_off(ahci_port, AHCI_PxCLB), (uint32_t)(uintptr_t)ahci_cmdlist);
    ahci_write32(ahci_port_off(ahci_port, AHCI_PxCLBU), 0);
    ahci_write32(ahci_port_off(ahci_port, AHCI_PxFB), (uint32_t)(uintptr_t)ahci_recv_fis);
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

/** Zero `n` bytes of a DMA region. */
static void ahci_zero(volatile uint8_t *p, uint32_t n)
{
    uint32_t i;

    for (i = 0; i < n; ++i) {
        p[i] = 0;
    }
}

/** Recover the port after a failed or timed-out command, then report `-EIO`.
 *
 * AHCI 6.2.2 error recovery: a fatal error stops the command engine and may
 * leave a command outstanding, so without recovery the next command would issue
 * on a stopped engine and wedge the port until a fresh `ahci_init`. Clearing
 * `PxCMD.ST` resets `PxCI`; once `CR` is clear the latched errors clear and the
 * engine restarts, leaving the port able to serve the next command. If the
 * engine will not stop, the port stays down and recovery fails closed rather
 * than restart a wedged engine. All bounded MMIO, so it is safe at IF=0. */
static int ahci_io_error(void)
{
    uint32_t cmd_off = ahci_port_off(ahci_port, AHCI_PxCMD);

    ahci_write32(cmd_off, ahci_read32(cmd_off) & ~AHCI_PxCMD_ST);
    if (ahci_wait_clear(cmd_off, AHCI_PxCMD_CR) != 0) {
        return -EIO;
    }
    ahci_write32(ahci_port_off(ahci_port, AHCI_PxSERR), 0xFFFFFFFFu);
    ahci_write32(ahci_port_off(ahci_port, AHCI_PxIS), 0xFFFFFFFFu);
    ahci_write32(cmd_off, ahci_read32(cmd_off) | AHCI_PxCMD_ST);
    return -EIO;
}

/** Issue one polled command on the selected port and spin to completion.
 *
 * Builds the command-list header, a Register H2D FIS, and a single PRDT entry
 * over the static DMA regions, waits for the task file to go idle, sets the
 * slot's `PxCI` bit, and busy-polls it clear, watching the fatal `PxIS` bits
 * and `PxTFD` ERR/DF for failure, then verifies the byte count the HBA reports
 * so a short transfer fails rather than passing. The HBA performs the DMA; the
 * CPU only spins
 * on memory-mapped status, bounded by the iteration watchdog. So it
 * neither sleeps nor allocates and is callable with interrupts off, the
 * contract the page-cache window fault needs. `buf` must be physically
 * contiguous and below 4 GiB, which every identity-mapped buffer below
 * HARD_MEMORY_LIMIT is. Returns 0 on success, `-EIO` on error or expiry. */
static int ahci_run_command(uint8_t command, uint64_t lba, uint32_t count, volatile uint8_t *buf,
                            uint32_t bytes, int write)
{
    volatile ahci_cmd_header_t *hdr = (volatile ahci_cmd_header_t *)ahci_cmdlist;
    volatile ahci_h2d_fis_t *fis = (volatile ahci_h2d_fis_t *)ahci_cmdtable;
    volatile ahci_prdt_t *prdt = (volatile ahci_prdt_t *)(ahci_cmdtable + AHCI_PRDT_OFFSET);
    uint32_t ci_off = ahci_port_off(ahci_port, AHCI_PxCI);
    uint32_t tfd_off = ahci_port_off(ahci_port, AHCI_PxTFD);
    uint32_t is_off = ahci_port_off(ahci_port, AHCI_PxIS);
    uint32_t prdtl = (bytes > 0) ? AHCI_PRDTL_ONE : 0u;
    uint32_t spin;

    /* The one place a command leaves this driver. A kernel built read-only
     * refuses here, whatever the caller believed it was doing. */
    if (!diskcmd_allowed(command)) {
        serial_printf("ahci: refused command 0x%X: %s\n", (unsigned)command, diskcmd_policy_banner);
        return -1;
    }

    ahci_zero(ahci_cmdtable, AHCI_CMDTABLE_SIZE);

    fis->fis_type = AHCI_FIS_H2D;
    fis->pmport_c = AHCI_FIS_H2D_C;
    fis->command = command;
    fis->featurel = 0;
    fis->lba0 = (uint8_t)lba;
    fis->lba1 = (uint8_t)(lba >> 8);
    fis->lba2 = (uint8_t)(lba >> 16);
    fis->device = ATA_DEV_LBA;
    fis->lba3 = (uint8_t)(lba >> 24);
    fis->lba4 = (uint8_t)(lba >> 32);
    fis->lba5 = (uint8_t)(lba >> 40);
    fis->featureh = 0;
    fis->countl = (uint8_t)count;
    fis->counth = (uint8_t)(count >> 8);
    fis->icc = 0;
    fis->control = 0;
    fis->aux = 0;

    if (bytes > 0) {
        prdt->dba = (uint32_t)(uintptr_t)buf;
        prdt->dbau = 0;
        prdt->reserved = 0;
        prdt->dbc = (bytes - 1) & AHCI_DBC_MASK;
    }

    hdr->dw0 = AHCI_CFL_DWORDS | (write ? AHCI_CMD_W : 0u) | prdtl;
    hdr->prdbc = 0;
    hdr->ctba = (uint32_t)(uintptr_t)ahci_cmdtable;
    hdr->ctbau = 0;

    ahci_dma_sync(ahci_cmdtable, AHCI_CMDTABLE_SIZE);
    ahci_dma_sync(ahci_cmdlist, sizeof(ahci_cmd_header_t));
    if (write && bytes > 0) {
        ahci_dma_sync(buf, bytes);
    }

    /* The task file must be idle before a command issues (AHCI 5.5.1). */
    spin = AHCI_CMD_STEPS;
    while (ahci_read32(tfd_off) & (AHCI_PxTFD_BSY | AHCI_PxTFD_DRQ)) {
        if (--spin == 0) {
            return ahci_io_error();
        }
        k_udelay(AHCI_CMD_POLL_US);
    }

    ahci_write32(is_off, 0xFFFFFFFFu); /* clear stale status (write 1) */
    block_count_command();
    if (command == ATA_FLUSH_EXT) {
        block_count_flush();
    }
    ahci_write32(ci_off, 1u << AHCI_SLOT);

    spin = AHCI_CMD_STEPS;
    while (ahci_read32(ci_off) & (1u << AHCI_SLOT)) {
        if (ahci_read32(is_off) & AHCI_PxIS_FATAL) {
            return ahci_io_error();
        }
        if (--spin == 0) {
            return ahci_io_error();
        }
        k_udelay(AHCI_CMD_POLL_US);
    }
    /* A fatal PxIS error can clear PxCI without setting PxTFD.ERR, so the
       post-loop check tests both: a fatal interrupt or a task-file ERR/DF. */
    if ((ahci_read32(is_off) & AHCI_PxIS_FATAL) ||
        (ahci_read32(tfd_off) & (AHCI_PxTFD_ERR | AHCI_PxTFD_DF))) {
        return ahci_io_error();
    }

    /* The HBA counts the bytes it moved into `prdbc`. A completion that moved
       fewer than requested without raising an error would otherwise pass, and a
       short write that pc_sync then marks clean is silent data loss. */
    if (bytes > 0) {
        ahci_dma_sync(ahci_cmdlist, sizeof(ahci_cmd_header_t));
        if (hdr->prdbc != bytes) {
            return ahci_io_error();
        }
    }

    if (!write && bytes > 0) {
        ahci_dma_sync(buf, bytes); /* evict stale lines; CPU reads fresh */
    }
    return 0;
}

/** IDENTIFY the device and record the 48-bit sector count (words 100-103).
   Rejects a device whose logical sector is not 512 bytes (IDENTIFY word 106
   bit 14 valid, bit 12 set), which the block math needs. Returns 0 on success,
   -1 on a failed IDENTIFY or a bad device. */
static int ahci_identify(void)
{
    static volatile uint8_t idbuf[BLOCK_SECTOR_SIZE] __attribute__((aligned(2)));
    volatile uint16_t *w = (volatile uint16_t *)idbuf;

    if (ahci_run_command(ATA_IDENTIFY, 0, 0, idbuf, BLOCK_SECTOR_SIZE, 0) != 0) {
        return -1;
    }
    if ((w[106] & 0x4000u) != 0 && (w[106] & 0x1000u) != 0) {
        return -1;
    }
    ahci_sector_count = (uint64_t)w[100] | ((uint64_t)w[101] << 16) | ((uint64_t)w[102] << 32) |
                        ((uint64_t)w[103] << 48);
    if (ahci_sector_count == 0) {
        ahci_err = "the device reported no sectors";
        return -1;
    }
    /* An all-ones capacity is a device that did not answer, not a 281 TB
     * disk: the field is 48 bits, so the top 16 must be zero, and a driver
     * that accepted it would truncate every LBA it later computed. */
    if ((ahci_sector_count >> 48) != 0 || ahci_sector_count == 0xFFFFFFFFFFFFull) {
        ahci_err = "the device reported an impossible capacity";
        return -1;
    }
    diskinfo_record_identify("ahci", ahci_port, w, ahci_sector_count);
    return 0;
}

/** Flush the device write cache so a completed write reaches durable media,
   the durability the dirty-flag and pc_sync write-back contract relies on. */
static int ahci_flush(void)
{
    return ahci_run_command(ATA_FLUSH_EXT, 0, 0, 0, 0, 0);
}

/** block0->read: a polled READ DMA EXT of `count` sectors from `lba`. */
static int ahci_read(block_device_t *device, uint64_t lba, uint32_t count, void *buffer)
{
    if (count == 0 || lba > device->sector_count || count > device->sector_count - lba ||
        count > AHCI_MAX_SECTORS_PER_CMD) {
        return -EIO;
    }
    return ahci_run_command(ATA_READ_DMA_EXT, lba, count, (volatile uint8_t *)buffer,
                            count * BLOCK_SECTOR_SIZE, 0);
}

/**
 * `block0->write`: a polled `WRITE DMA EXT`, then `FLUSH CACHE EXT` for
 * durability.
 */
static int ahci_write(block_device_t *device, uint64_t lba, uint32_t count, const void *buffer)
{
    int rc;

    if (count == 0 || lba > device->sector_count || count > device->sector_count - lba ||
        count > AHCI_MAX_SECTORS_PER_CMD) {
        return -EIO;
    }
    rc = ahci_run_command(ATA_WRITE_DMA_EXT, lba, count, (volatile uint8_t *)(uintptr_t)buffer,
                          count * BLOCK_SECTOR_SIZE, 1);
    if (rc != 0) {
        return rc;
    }
    return ahci_flush();
}

static block_device_t ahci_device;

int ahci_init(void)
{
    /* Leave the published device unset until bring-up succeeds, so a failed
     * probe fails closed regardless of caller (mirrors ata_init). */
    block0 = 0;
    ahci_base = 0;
    ahci_port = -1;
    ahci_sector_count = 0;
    ahci_detect_cache();
    if (ahci_discover() != 0 || ahci_hba_own() != 0 || ahci_quiesce_ports() != 0 ||
        ahci_select_port() != 0 || ahci_port_start() != 0 || ahci_identify() != 0) {
        serial_printf("ahci: %s\n", ahci_err);
        /* The registers behind the refusal, into the log the machine hands
         * out afterwards (/dev/bootlog): one failed boot then carries enough
         * to decide between every cause the investigation named, instead of
         * costing another trip to the machine. */
        if (ahci_base) {
            ahci_dump();
        }
        return -1;
    }

    ahci_device.sector_size = BLOCK_SECTOR_SIZE;
    ahci_device.sector_count = ahci_sector_count;
    ahci_device.read = ahci_read;
    ahci_device.write = ahci_write;
    ahci_device.drvdata = 0;
    block0 = &ahci_device;

    serial_printf("ahci: port %d: %d sectors, %d MiB\n", ahci_port, (uint32_t)ahci_sector_count,
                  (uint32_t)(ahci_sector_count / 2048));
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
    serial_printf("ahci: CAP=%X GHC=%X PI=%X VS=%X ports=%d active=%d\n", cap, ghc, pi, vs,
                  (int)((cap & AHCI_CAP_NP_MASK) + 1), ahci_port);
    /* CAP2 says whether a handoff exists, BOHC how it went, and PCS whether
     * the Intel port PHYs are enabled at all; each is one of the named causes
     * of a machine that finds no disk, so the dump that decides between them
     * has to carry all three. */
    serial_printf("ahci: CAP2=%X BOHC=%X PCS=%X\n", ahci_read32(AHCI_CAP2), ahci_read32(AHCI_BOHC),
                  (unsigned)pci_read_config16(ahci_pci.bus, ahci_pci.device, ahci_pci.function,
                                              AHCI_INTEL_PCS));
    serial_printf("ahci: dma cmdlist=%X fis=%X table=%X\n", (uint32_t)(uintptr_t)ahci_cmdlist,
                  (uint32_t)(uintptr_t)ahci_recv_fis, (uint32_t)(uintptr_t)ahci_cmdtable);

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
        serial_printf("ahci:  port %d: SSTS=%X (DET=%d) "
                      "SIG=%X TFD=%X CMD=%X CLB=%X\n",
                      port, ssts, (int)(ssts & AHCI_DET_MASK), sig, tfd, cmd, clb);
        serial_printf("ahci:  port %d: SCTL=%X SERR=%X\n", port,
                      ahci_read32(ahci_port_off(port, AHCI_PxSCTL)),
                      ahci_read32(ahci_port_off(port, AHCI_PxSERR)));
    }
}

void ahci_selftest(void)
{
    static uint8_t buf[BLOCK_SECTOR_SIZE] __attribute__((aligned(2)));

    if (!block0) {
        serial_printf("ahci: selftest: not initialized\n");
        return;
    }
    if (block0->read(block0, 0, 1, buf) != 0) {
        serial_printf("ahci: selftest: lba 0 read failed\n");
        return;
    }
    serial_printf("ahci: lba 0 signature %X %X\n", (uint32_t)buf[510], (uint32_t)buf[511]);
    if (buf[510] == 0x55 && buf[511] == 0xAA) {
        serial_printf("ahci: selftest: MBR signature OK\n");
    } else {
        serial_printf("ahci: selftest: MBR signature mismatch\n");
    }
}
