#include "../klog.h"
#include "../lib/kcpu.h"
#include "ahci.h"
#include "diskcmd.h"
#include "disk.h"
#include "../udelay.h"
#include "block.h"

#include "../pci.h"
#include "../serial.h"
#include "../mm/paging.h"
#include "../isr.h"
#include "../lapic.h"
#include "../lib/kirq.h"
#include "../pit.h"
#include "../cmdline.h"
#include "../latency.h"

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
#define AHCI_IS   0x08 /* interrupt status, one bit per port */
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
   and why Linux has a named quirk for it. */
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

/* Every wait is measured against the timer chip, not counted, because
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

/* The two waits that follow a COMRESET are measured, not counted, since
   what they wait for happens on the wire and in the device, not at the
   processor's speed. The poll interval is 100 us; the budgets are one second
   for the PHY to negotiate, which is what the specification allows, and five
   for the device to finish its own reset and post its signature, which is
   generous for a disk and short enough that a port with something unresponsive
   on it does not hold up a boot. Both expiries are ordinary answers: an empty
   port and a device that will not answer are both ports to move past. */
#define AHCI_RESET_POLL_US 100u
#define AHCI_LINK_STEPS    10000u
#define AHCI_READY_STEPS   50000u

/* The DMA regions the HBA reads and writes, one set per disk. They are
   static so they land in .bss below HARD_MEMORY_LIMIT, where the identity map
   makes the virtual address equal the physical DMA address and the bytes are
   contiguous and below 4 GiB. The alignments are AHCI-mandated. */
#define AHCI_CMDLIST_SIZE  1024 /* 32 command headers of 32 bytes */
#define AHCI_RECV_FIS_SIZE 256
#define AHCI_CMDTABLE_SIZE 256 /* 128-byte header plus a 16-byte PRDT entry */

/* How many disks one controller may publish. A port beyond the count is
   stopped and left, with a record. */
#define AHCI_DISKS 4

/**
 * One disk on one port: its DMA areas, its capacity, and the device the
 * block layer sees, whose `drvdata` points back here.
 */
struct ahci_disk {
    volatile uint8_t cmdlist[AHCI_CMDLIST_SIZE] __attribute__((aligned(1024)));
    volatile uint8_t recv_fis[AHCI_RECV_FIS_SIZE] __attribute__((aligned(256)));
    volatile uint8_t cmdtable[AHCI_CMDTABLE_SIZE] __attribute__((aligned(128)));
    int port;
    uint64_t sectors; /* IDENTIFY words 100-103, 0 until known */
    block_device_t dev;
    /* The interrupt-driven path: one command active on the port, the
     * request it belongs to, and the requests waiting their turn, oldest
     * first. A write is two commands, the transfer and then the flush, and
     * `flushing` says which the active one is. */
    struct block_request *active;
    uint32_t active_generation;
    int flushing;
    struct block_request *queue_head, *queue_tail;
};

static struct ahci_disk ahci_disks[AHCI_DISKS];
static int ahci_ndisks;

/* The vector the controller's messages arrive on once `ahci_start` has
   switched the disks to the interrupt-driven path, -1 before and under
   `rach2.ahcipoll`, which keeps every transfer polled. */
static int ahci_vector = -1;
static uint32_t ahci_messages;

/* The areas a port is pointed at while it is reset, before the scan knows
   whether it has a disk. Nothing reads them: a port that is kept gets its
   own areas when it is started, and a port that is skipped is stopped. */
static volatile uint8_t ahci_reset_cmdlist[AHCI_CMDLIST_SIZE] __attribute__((aligned(1024)));
static volatile uint8_t ahci_reset_fis[AHCI_RECV_FIS_SIZE] __attribute__((aligned(256)));

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
/* The port interrupts the driven path enables: a D2H register FIS, which
   every command ends with, and the fatal ones. */
#define AHCI_PxIS_DHRS  0x00000001u
#define AHCI_PxIE_WANTED (AHCI_PxIS_DHRS | AHCI_PxIS_FATAL)

/* The H2D Register FIS and the ATA commands the polled path issues. */
#define AHCI_FIS_H2D      0x27u
#define AHCI_FIS_H2D_C    0x80u /* C bit: this FIS has a command */
#define ATA_READ_DMA_EXT  0x25u
#define ATA_WRITE_DMA_EXT 0x35u
#define ATA_FLUSH_EXT     0xEAu /* FLUSH CACHE EXT, durability after a write */
#define ATA_IDENTIFY      0xECu
#define ATA_DEV_LBA       0x40u /* device register: LBA mode */

/* One PRDT entry transfers up to 4 MiB, so cap a single command there; every
   exercised caller (the page cache, 8 sectors) is far under it, and a larger
   request fails closed, not silently truncating the byte count. */
#define AHCI_MAX_SECTORS_PER_CMD 8192u

/* The single polled command slot, its 5-dword H2D FIS, and the PRDT offset
   (after CFIS 64 + ACMD 16 + reserved 48) inside the command table. */
#define AHCI_SLOT        0
#define AHCI_CFL_DWORDS  5
#define AHCI_CMD_W       (1u << 6)  /* command header: write direction */
#define AHCI_PRDTL_ONE   (1u << 16) /* command header: one PRDT entry */
#define AHCI_PRDT_OFFSET 0x80
/* The entries the command table has room for after the FIS: eight, so a
 * buffer from the heap, one entry per page, may span 32 KiB. */
#define AHCI_PRDT_MAX    ((AHCI_CMDTABLE_SIZE - AHCI_PRDT_OFFSET) / 16)
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

/* The mapped ABAR, NULL until ahci_init succeeds. MMIO, so volatile. The
   first port with a disk, -1 until bring-up. */
static pci_device_t ahci_pci;
static volatile uint8_t *ahci_base;
static int ahci_port = -1;

/*
 * Everything this driver does to the outside world, in one table.
 *
 * On a machine it is the four lines below: two memory reads and writes through
 * the mapped ABAR, the delay, and the PCI configuration accessors. A test on
 * another architecture cannot map an ABAR or answer a PCI read, so it supplies
 * its own table and the driver runs against a model of the controller. That is
 * the only reason the indirection exists, and it is the same arrangement the
 * e1000 driver has.
 */
static uint32_t ahci_mmio_r32(uint32_t offset)
{
    return *(volatile uint32_t *)(ahci_base + offset);
}

static void ahci_mmio_w32(uint32_t offset, uint32_t value)
{
    *(volatile uint32_t *)(ahci_base + offset) = value;
}

static const struct ahci_io ahci_mmio_io = {ahci_mmio_r32, ahci_mmio_w32, k_udelay};

static const struct ahci_io *ahci_io = &ahci_mmio_io;

void ahci_set_io(const struct ahci_io *ops)
{
    ahci_io = ops ? ops : &ahci_mmio_io;
}

/** Read the 32-bit ABAR register at `offset`. */
static uint32_t ahci_read32(uint32_t offset)
{
    return ahci_io->r32(offset);
}

/** Write `value` to the 32-bit ABAR register at `offset`. */
static void ahci_write32(uint32_t offset, uint32_t value)
{
    ahci_io->w32(offset, value);
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

/**
 * Record whether CLFLUSH is available and its line size (CPUID leaf 1).
 *
 * The two instructions this needs are x86's, so a test compiled for another
 * architecture cannot run it. There is nothing to model either: what it
 * decides is whether the flush below does anything, and on a machine whose
 * DMA snoops the caches the answer costs nothing. It is left out where the
 * instructions do not exist, and the flush is then the no-op it already is on
 * every emulator this project runs on.
 */
static void ahci_detect_cache(void)
{
#if defined(__i386__) || defined(__x86_64__)
    uint32_t eax = 0, ebx = 0, ecx = 0, edx = 0;
    uint32_t sz;

    kcpu_id(1, &eax, &ebx, &ecx, &edx);

    ahci_have_clflush = ((edx & (1u << 19)) != 0);
    sz = ((ebx >> 8) & 0xFFu) * 8u;
    if (sz == 0) {
        sz = 64;
    }
    ahci_clflush_size = sz;
#else
    ahci_have_clflush = false;
    ahci_clflush_size = 64;
#endif
}

/** Evict [addr, addr+size) so a DMA write the CPU is about to read, or a
   descriptor the HBA is about to read, crosses the cache a line at a time. */
static void ahci_dma_sync(const volatile void *addr, uint32_t size)
{
    if (!addr || size == 0) {
        return;
    }
#if defined(__i386__) || defined(__x86_64__)
    if (ahci_have_clflush && ahci_clflush_size != 0) {
        uintptr_t line = (uintptr_t)ahci_clflush_size;
        uintptr_t p = (uintptr_t)addr & ~(line - 1u);
        uintptr_t end = (uintptr_t)addr + (uintptr_t)size;

        while (p < end) {
            /* clflush (%eax) as bytes 0f ae 38: tcc has no mnemonic for it. */
            __asm__ __volatile__(".byte 0x0f, 0xae, 0x38" ::"a"(p) : "memory");
            p += line;
        }
        __asm__ __volatile__("lock; addl $0, 0(%%esp)" ::: "memory");
    } else {
        __asm__ __volatile__("wbinvd" ::: "memory");
    }
#else
    /* The instructions above are x86's, and where they do not exist there is
       no controller either: what runs there is a model of one. */
    (void)addr;
    (void)size;
#endif
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
        ahci_io->udelay(AHCI_WAIT_POLL_US);
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
    uintptr_t mapped;
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
        klog(KLOG_DEBUG, "ahci",
             "no match: 0:31.2 reads %X:%X class %X:%X prog-if %X",
                      (unsigned)pci_read_config16(0, 31, 2, PCI_VENDOR_ID),
                      (unsigned)pci_read_config16(0, 31, 2, PCI_DEVICE_ID),
                      (unsigned)pci_read_config8(0, 31, 2, PCI_CLASS),
                      (unsigned)pci_read_config8(0, 31, 2, PCI_SUBCLASS),
                      (unsigned)pci_read_config8(0, 31, 2, PCI_PROG_IF));
        ahci_err = "no SATA controller in AHCI mode on the bus";
        return -1;
    }

    klog(KLOG_DEBUG, "ahci", "found: %X:%X at %d:%d.%d, prog-if %X, BAR5 %X",
         (unsigned)dev.vendor_id, (unsigned)dev.device_id, (int)dev.bus,
         (int)dev.device, (int)dev.function, (unsigned)dev.prog_if,
         (unsigned)dev.bar[5]);

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
    /* Map the ABAR cache-disabled (PCD|PWT) whatever its address: map_mmio
       gives it a mapping of its own in the kernel's MMIO region, and the
       driver reads and writes through that one, never through a low ABAR's
       identity mapping, which stays cacheable. */
    mapped = map_mmio(abar, AHCI_ABAR_MAP_SIZE);
    if (mapped == 0) {
        ahci_err = "the ABAR window could not be mapped";
        return -1;
    }
    ahci_base = (volatile uint8_t *)mapped;

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
 * and the only way back is to take the link down and bring it up: DET=1 has
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
     * the received-FIS area is programmed and FRE turned on before the reset,
     * not in the start path afterwards; without this the task file
     * never updates and every port looks like a device that will not answer.
     * The command engine stays off: nothing is being issued here. */
    ahci_write32(ahci_port_off(port, AHCI_PxCLB), (uint32_t)(uintptr_t)ahci_reset_cmdlist);
    ahci_write32(ahci_port_off(port, AHCI_PxCLBU), 0);
    ahci_write32(ahci_port_off(port, AHCI_PxFB), (uint32_t)(uintptr_t)ahci_reset_fis);
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
     * a warning, not a refusal: Linux never performs this handoff at
     * all (BOHC appears in its AHCI driver only as a capability bit's name),
     * so firmware with a broken handoff semaphore would fail only us, on a
     * controller Linux would simply take. Asking first is still the polite
     * order; being refused by silence is not a reason to stop. */
    if (ahci_read32(AHCI_CAP2) & AHCI_CAP2_BOH) {
        ahci_write32(AHCI_BOHC, ahci_read32(AHCI_BOHC) | AHCI_BOHC_OOS);
        if (ahci_wait_clear(AHCI_BOHC, AHCI_BOHC_BOS) != 0) {
            klog(KLOG_WARN, "ahci",
                 "handoff: the firmware did not answer; continuing");
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
            klog(KLOG_DEBUG, "ahci", "pcs: enabled ports %X (was %X)",
                 (unsigned)ports, (unsigned)pcs);
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
 * and the signature register has nothing worth reading. Bounded, because a
 * device that never becomes ready is a device this driver must move past,
 * not wait on.
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
 * The signature decides what the port has, and only a disk is taken. An
 * optical drive answers the ATAPI signature and would fail every command this
 * driver knows; a port multiplier and an enclosure service answer their own.
 * Naming the one signature this driver drives instead of excluding the ones
 * it happens to know about, means an unfamiliar device is skipped rather than
 * driven as if it were a disk.
 *
 * A port that is skipped is stopped again before the scan moves on. The reset
 * had to enable that port's FIS receive engine, and a port left running with
 * no area of its own programmed would write wherever its base registers
 * point. On the machine this targets that is not hypothetical: it has a SATA
 * disk and a bay that may hold an optical drive, which is exactly two ports.
 *
 * Every port with a disk is kept, up to `AHCI_DISKS`, in port order; the
 * first is `ahci_port`. The function fails only when no port has a disk.
 */
static int ahci_select_port(void)
{
    uint32_t pi = ahci_read32(AHCI_PI);
    int p;

    ahci_ndisks = 0;
    ahci_port = -1;
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
            klog(KLOG_WARN, "ahci",
                 "port %d: the device never became ready; skipped", p);
            (void)ahci_port_stop(p);
            continue;
        }
        sig = ahci_read32(ahci_port_off(p, AHCI_PxSIG));
        if (sig != AHCI_SIG_ATA) {
            klog(KLOG_DEBUG, "ahci",
                 "port %d: signature %X is not a disk; skipped", p, sig);
            (void)ahci_port_stop(p);
            continue;
        }
        if (ahci_ndisks >= AHCI_DISKS) {
            klog(KLOG_WARN, "ahci", "port %d: a disk past the %d this driver publishes; skipped",
                 p, AHCI_DISKS);
            (void)ahci_port_stop(p);
            continue;
        }
        ahci_disks[ahci_ndisks].port = p;
        ahci_disks[ahci_ndisks].sectors = 0;
        ahci_ndisks++;
        if (ahci_port < 0) {
            ahci_port = p;
        }
    }
    if (ahci_ndisks == 0) {
        ahci_err = "no port has a disk";
        return -1;
    }
    return 0;
}

/** Stop `d`'s port engine, program its DMA bases, and restart it. */
static int ahci_port_start(struct ahci_disk *d)
{
    int port = d->port;
    uint32_t cmd_off = ahci_port_off(port, AHCI_PxCMD);

    if (ahci_port_stop(port) != 0) {
        ahci_err = "the selected port would not stop";
        return -1;
    }

    /* Program the command list and received-FIS bases (32-bit; upper = 0). */
    ahci_write32(ahci_port_off(port, AHCI_PxCLB), (uint32_t)(uintptr_t)d->cmdlist);
    ahci_write32(ahci_port_off(port, AHCI_PxCLBU), 0);
    ahci_write32(ahci_port_off(port, AHCI_PxFB), (uint32_t)(uintptr_t)d->recv_fis);
    ahci_write32(ahci_port_off(port, AHCI_PxFBU), 0);

    /* Clear latched errors and interrupt status (write 1 to clear). */
    ahci_write32(ahci_port_off(port, AHCI_PxSERR), 0xFFFFFFFFu);
    ahci_write32(ahci_port_off(port, AHCI_PxIS), 0xFFFFFFFFu);

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
static int ahci_io_error(struct ahci_disk *d)
{
    uint32_t cmd_off = ahci_port_off(d->port, AHCI_PxCMD);

    ahci_write32(cmd_off, ahci_read32(cmd_off) & ~AHCI_PxCMD_ST);
    if (ahci_wait_clear(cmd_off, AHCI_PxCMD_CR) != 0) {
        return -EIO;
    }
    ahci_write32(ahci_port_off(d->port, AHCI_PxSERR), 0xFFFFFFFFu);
    ahci_write32(ahci_port_off(d->port, AHCI_PxIS), 0xFFFFFFFFu);
    ahci_write32(cmd_off, ahci_read32(cmd_off) | AHCI_PxCMD_ST);
    return -EIO;
}

/** Issue one polled command on `d`'s port and spin to completion.
 *
 * Builds the command-list header, a Register H2D FIS, and a single PRDT entry
 * over the disk's DMA regions, waits for the task file to go idle, sets the
 * slot's `PxCI` bit, and busy-polls it clear, watching the fatal `PxIS` bits
 * and `PxTFD` ERR/DF for failure, then verifies the byte count the HBA reports
 * so a short transfer fails instead of passing. The HBA performs the DMA; the
 * CPU only spins
 * on memory-mapped status, bounded by the iteration watchdog. So it
 * neither sleeps nor allocates and is callable with interrupts off, the
 * contract the page-cache window fault needs. A `buf` below
 * HARD_MEMORY_LIMIT is identity-mapped and physically contiguous, and goes
 * in one PRDT entry; one above it is a heap buffer, whose pages may lie
 * anywhere, and gets an entry per page with the page's physical address,
 * up to the eight the table holds. Returns 0 on success, `-EIO` on error,
 * expiry, or a buffer the table cannot describe. */
/**
 * Build the command in `d`'s slot: the header, the FIS and the PRDT, and
 * sync them for the HBA. Returns 0, or -1 when the policy refuses the
 * command or the table cannot describe the buffer.
 */
static int ahci_build_command(struct ahci_disk *d, uint8_t command, uint64_t lba, uint32_t count,
                              volatile uint8_t *buf, uint32_t bytes, int write)
{
    volatile ahci_cmd_header_t *hdr = (volatile ahci_cmd_header_t *)d->cmdlist;
    volatile ahci_h2d_fis_t *fis = (volatile ahci_h2d_fis_t *)d->cmdtable;
    volatile ahci_prdt_t *prdt = (volatile ahci_prdt_t *)(d->cmdtable + AHCI_PRDT_OFFSET);
    uint32_t prdtl = 0;

    /* The one place a command leaves this driver. A kernel built read-only
     * refuses here, whatever the caller believed it was doing. */
    if (!diskcmd_allowed(command)) {
        klog(KLOG_WARN, "ahci", "refused: command 0x%X: %s", (unsigned)command,
             diskcmd_policy_banner);
        return -1;
    }

    ahci_zero(d->cmdtable, AHCI_CMDTABLE_SIZE);

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

    if (bytes > 0 && (uintptr_t)buf < HARD_MEMORY_LIMIT) {
        prdt->dba = (uint32_t)(uintptr_t)buf;
        prdt->dbau = 0;
        prdt->reserved = 0;
        prdt->dbc = (bytes - 1) & AHCI_DBC_MASK;
        prdtl = AHCI_PRDTL_ONE;
    } else if (bytes > 0) {
        uint32_t off = 0, n = 0;

        while (off < bytes) {
            uintptr_t va = (uintptr_t)buf + off;
            uintptr_t phys = paging_virt_to_phys(va);
            uint32_t chunk = PAGE_SIZE - (uint32_t)(va & (PAGE_SIZE - 1));

            if (chunk > bytes - off) {
                chunk = bytes - off;
            }
            if (phys == 0 || n >= AHCI_PRDT_MAX) {
                klog(KLOG_WARN, "ahci", "refused: a buffer the command table cannot describe");
                return -1;
            }
            prdt[n].dba = (uint32_t)phys;
            prdt[n].dbau = 0;
            prdt[n].reserved = 0;
            prdt[n].dbc = (chunk - 1) & AHCI_DBC_MASK;
            n++;
            off += chunk;
        }
        prdtl = n << 16;
    }

    hdr->dw0 = AHCI_CFL_DWORDS | (write ? AHCI_CMD_W : 0u) | prdtl;
    hdr->prdbc = 0;
    hdr->ctba = (uint32_t)(uintptr_t)d->cmdtable;
    hdr->ctbau = 0;

    ahci_dma_sync(d->cmdtable, AHCI_CMDTABLE_SIZE);
    ahci_dma_sync(d->cmdlist, sizeof(ahci_cmd_header_t));
    if (write && bytes > 0) {
        ahci_dma_sync(buf, bytes);
    }
    return 0;
}

/**
 * Judge the command that has left the slot: 0 when it completed and moved
 * `bytes`, `-EIO` after the port's recovery otherwise. The HBA counts the
 * bytes it moved into `prdbc`; a completion that moved fewer than requested
 * without raising an error would otherwise pass, and a short write that
 * pc_sync then marks clean is silent data loss. A fatal PxIS error can
 * clear PxCI without setting PxTFD.ERR, so both are tested.
 */
static int ahci_judge(struct ahci_disk *d, uint32_t bytes, volatile uint8_t *buf, int write)
{
    volatile ahci_cmd_header_t *hdr = (volatile ahci_cmd_header_t *)d->cmdlist;
    uint32_t tfd_off = ahci_port_off(d->port, AHCI_PxTFD);
    uint32_t is_off = ahci_port_off(d->port, AHCI_PxIS);

    if ((ahci_read32(is_off) & AHCI_PxIS_FATAL) ||
        (ahci_read32(tfd_off) & (AHCI_PxTFD_ERR | AHCI_PxTFD_DF))) {
        return ahci_io_error(d);
    }
    if (bytes > 0) {
        ahci_dma_sync(d->cmdlist, sizeof(ahci_cmd_header_t));
        if (hdr->prdbc != bytes) {
            return ahci_io_error(d);
        }
    }
    if (!write && bytes > 0) {
        ahci_dma_sync(buf, bytes); /* evict stale lines; CPU reads fresh */
    }
    return 0;
}

static void ahci_drain(struct ahci_disk *d);
static void ahci_start_next(struct ahci_disk *d);

static int ahci_run_command(struct ahci_disk *d, uint8_t command, uint64_t lba, uint32_t count,
                            volatile uint8_t *buf, uint32_t bytes, int write)
{
    uint32_t ci_off = ahci_port_off(d->port, AHCI_PxCI);
    uint32_t tfd_off = ahci_port_off(d->port, AHCI_PxTFD);
    uint32_t is_off = ahci_port_off(d->port, AHCI_PxIS);
    uint32_t spin;
    int rc;

    /* A polled command on a port with a queued one active: the fault path
     * with interrupts off, or a raw window on the recovery kernel. The
     * active command is finished here first, its request completed, and
     * the queue resumes once this command is done. */
    ahci_drain(d);
    if (ahci_build_command(d, command, lba, count, buf, bytes, write) != 0) {
        return -1;
    }

    /* The task file must be idle before a command issues (AHCI 5.5.1). */
    spin = AHCI_CMD_STEPS;
    while (ahci_read32(tfd_off) & (AHCI_PxTFD_BSY | AHCI_PxTFD_DRQ)) {
        if (--spin == 0) {
            return ahci_io_error(d);
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
            return ahci_io_error(d);
        }
        if (--spin == 0) {
            return ahci_io_error(d);
        }
        k_udelay(AHCI_CMD_POLL_US);
    }
    rc = ahci_judge(d, bytes, buf, write);
    ahci_start_next(d);
    return rc;
}

/*
 * The interrupt-driven path.
 *
 * A request is at most a page in the identity-mapped window, so its page is
 * one PRDT entry with the physical address the virtual one is. One command
 * is active per port; the rest wait in the port's queue. The port's
 * interrupt completes the active request and starts the next, so the
 * processor runs someone else during a transfer and two disks progress
 * independently. A write is the transfer and then a flush, so a completed
 * write is on the medium as the polled path's is.
 */

/** Issue the command built in `d`'s slot without waiting. */
static void ahci_issue(struct ahci_disk *d)
{
    uint32_t is_off = ahci_port_off(d->port, AHCI_PxIS);

    ahci_write32(is_off, 0xFFFFFFFFu);
    block_count_command();
    ahci_write32(ahci_port_off(d->port, AHCI_PxCI), 1u << AHCI_SLOT);
}

/**
 * Start the request `r` on `d`: a read, or the transfer half of a write.
 * Returns 0, or -1 when the command could not be built.
 */
static int ahci_start_request(struct ahci_disk *d, struct block_request *r)
{
    uint32_t bytes = r->count * BLOCK_SECTOR_SIZE;

    if (ahci_build_command(d, r->write ? ATA_WRITE_DMA_EXT : ATA_READ_DMA_EXT, r->lba, r->count,
                           (volatile uint8_t *)r->page, bytes, r->write) != 0) {
        return -1;
    }
    d->active = r;
    d->active_generation = r->generation;
    d->flushing = 0;
    ahci_issue(d);
    return 0;
}

/**
 * The active command has left the slot: judge it, finish its request or
 * start the flush half of a write, and start the next request queued.
 * Runs from the interrupt, and from the polled path draining the port.
 */
static void ahci_finish_active(struct ahci_disk *d)
{
    struct block_request *r = d->active;
    int rc;

    if (!r) {
        return;
    }
    rc = ahci_judge(d, d->flushing ? 0 : r->count * BLOCK_SECTOR_SIZE, (volatile uint8_t *)r->page,
                    r->write);
    if (rc == 0 && r->write && !d->flushing) {
        if (ahci_build_command(d, ATA_FLUSH_EXT, 0, 0, 0, 0, 0) == 0) {
            d->flushing = 1;
            block_count_flush();
            ahci_issue(d);
            return;
        }
        rc = -EIO;
    }
    d->active = 0;
    latency_note(LAT_DISK, pit_now_us() - r->taken_us);
    block_request_complete(r, d->active_generation, rc);
}

/** Start the oldest queued request when the port is idle. */
static void ahci_start_next(struct ahci_disk *d)
{
    while (!d->active && d->queue_head) {
        struct block_request *r = d->queue_head;

        d->queue_head = r->queued;
        if (!d->queue_head) {
            d->queue_tail = 0;
        }
        r->queued = 0;
        if (ahci_start_request(d, r) != 0) {
            block_request_complete(r, r->generation, -EIO);
        }
    }
}

/**
 * Finish the active command by polling, for a caller that needs the port
 * now and cannot take an interrupt: the fault path with interrupts off.
 * The interrupt that would have completed it finds nothing active.
 */
static void ahci_drain(struct ahci_disk *d)
{
    uint32_t ci_off = ahci_port_off(d->port, AHCI_PxCI);
    uint32_t is_off = ahci_port_off(d->port, AHCI_PxIS);
    uint32_t flags;

    if (!d->active) {
        return;
    }
    flags = kirq_save();
    while (d->active) {
        uint32_t spin = AHCI_CMD_STEPS;

        while ((ahci_read32(ci_off) & (1u << AHCI_SLOT)) &&
               !(ahci_read32(is_off) & AHCI_PxIS_FATAL)) {
            if (--spin == 0) {
                break;
            }
            k_udelay(AHCI_CMD_POLL_US);
        }
        if (spin == 0) {
            struct block_request *r = d->active;

            (void)ahci_io_error(d);
            d->active = 0;
            block_request_complete(r, d->active_generation, -EIO);
            break;
        }
        ahci_finish_active(d);
    }
    kirq_restore(flags);
}

/** The device's `submit`: queue `r`, and start it when the port is idle. */
static int ahci_submit(block_device_t *device, struct block_request *r)
{
    struct ahci_disk *d = (struct ahci_disk *)device->drvdata;
    uint32_t flags;

    if (r->count == 0 || r->lba > device->sector_count || r->count > device->sector_count - r->lba) {
        return -EIO;
    }
    flags = kirq_save();
    r->queued = 0;
    if (d->queue_tail) {
        d->queue_tail->queued = r;
    } else {
        d->queue_head = r;
    }
    d->queue_tail = r;
    ahci_start_next(d);
    kirq_restore(flags);
    return 0;
}

/**
 * The controller's interrupt: every port with a disk whose command has
 * left the slot is finished, and its interrupt status cleared.
 */
static void ahci_handler(isr_params_t *params)
{
    int i;

    (void)params;
    ahci_messages++;
    for (i = 0; i < ahci_ndisks; i++) {
        struct ahci_disk *d = &ahci_disks[i];
        uint32_t is_off = ahci_port_off(d->port, AHCI_PxIS);
        uint32_t is = ahci_read32(is_off);

        if (is == 0) {
            continue;
        }
        if (d->active && !(ahci_read32(ahci_port_off(d->port, AHCI_PxCI)) & (1u << AHCI_SLOT))) {
            ahci_finish_active(d);
            /* The status this interrupt reported is cleared before the next
             * command is issued. Acknowledging afterwards clears the bit
             * that command has already set when it finishes quickly, and
             * the next interrupt then finds nothing to do and leaves it
             * active until its deadline. */
            ahci_write32(is_off, is);
            ahci_start_next(d);
            continue;
        }
        if (d->active && (is & AHCI_PxIS_FATAL)) {
            struct block_request *r = d->active;

            (void)ahci_io_error(d);
            d->active = 0;
            block_request_complete(r, d->active_generation, -EIO);
            ahci_write32(is_off, is);
            ahci_start_next(d);
            continue;
        }
        ahci_write32(is_off, is);
    }
    ahci_write32(AHCI_IS, ahci_read32(AHCI_IS));
}

void ahci_start(void)
{
    int i;

    if (!ahci_base || ahci_ndisks == 0 || kcmdline_has("rach2.ahcipoll")) {
        return;
    }
    if (!lapic_ready() || (ahci_vector = isr_alloc_vector()) < 0) {
        klog(KLOG_INFO, "ahci", "polled: no vector for the controller's messages");
        return;
    }
    register_isr_handler((uint8_t)ahci_vector, ahci_handler, "ahci");
    if (pci_msi_enable(&ahci_pci, (uint8_t)ahci_vector) != 0) {
        klog(KLOG_INFO, "ahci", "polled: the controller has no message capability");
        ahci_vector = -1;
        return;
    }
    for (i = 0; i < ahci_ndisks; i++) {
        struct ahci_disk *d = &ahci_disks[i];

        ahci_write32(ahci_port_off(d->port, AHCI_PxIS), 0xFFFFFFFFu);
        ahci_write32(ahci_port_off(d->port, AHCI_PxIE), AHCI_PxIE_WANTED);
        d->dev.flags &= ~BLOCK_FAULT_SAFE;
        d->dev.submit = ahci_submit;
    }
    ahci_write32(AHCI_IS, 0xFFFFFFFFu);
    ahci_write32(AHCI_GHC, ahci_read32(AHCI_GHC) | AHCI_GHC_IE);
    klog(KLOG_INFO, "ahci", "interrupts: messages on vector %d, %d disks queued", ahci_vector,
         ahci_ndisks);
}

uint32_t ahci_interrupts(void)
{
    return ahci_messages;
}

/* The last IDENTIFY answer, which the registry reads the strings from. */
static volatile uint8_t ahci_idbuf[BLOCK_SECTOR_SIZE] __attribute__((aligned(2)));

/** IDENTIFY `d` and record its 48-bit sector count (words 100-103).
   Rejects a device whose logical sector is not 512 bytes (IDENTIFY word 106
   bit 14 valid, bit 12 set), which the block math needs. Returns 0 on success,
   -1 on a failed IDENTIFY or a bad device. */
static int ahci_identify(struct ahci_disk *d)
{
    volatile uint16_t *w = (volatile uint16_t *)ahci_idbuf;

    if (ahci_run_command(d, ATA_IDENTIFY, 0, 0, ahci_idbuf, BLOCK_SECTOR_SIZE, 0) != 0) {
        ahci_err = "the device did not answer IDENTIFY";
        return -1;
    }
    if ((w[106] & 0x4000u) != 0 && (w[106] & 0x1000u) != 0) {
        ahci_err = "the device's logical sector is not 512 bytes";
        return -1;
    }
    d->sectors = (uint64_t)w[100] | ((uint64_t)w[101] << 16) | ((uint64_t)w[102] << 32) |
                 ((uint64_t)w[103] << 48);
    if (d->sectors == 0) {
        ahci_err = "the device reported no sectors";
        return -1;
    }
    /* An all-ones capacity is a device that did not answer, not a 281 TB
     * disk: the field is 48 bits, so the top 16 must be zero, and a driver
     * that accepted it would truncate every LBA it later computed. */
    if ((d->sectors >> 48) != 0 || d->sectors == 0xFFFFFFFFFFFFull) {
        ahci_err = "the device reported an impossible capacity";
        return -1;
    }
    return 0;
}

/** Flush the device write cache so a completed write reaches durable media,
   the durability the dirty-flag and pc_sync write-back contract relies on. */
static int ahci_flush(struct ahci_disk *d)
{
    return ahci_run_command(d, ATA_FLUSH_EXT, 0, 0, 0, 0, 0);
}

/** The device's read: a polled READ DMA EXT of `count` sectors from `lba`. */
static int ahci_read(block_device_t *device, uint64_t lba, uint32_t count, void *buffer)
{
    struct ahci_disk *d = (struct ahci_disk *)device->drvdata;

    if (count == 0 || lba > device->sector_count || count > device->sector_count - lba ||
        count > AHCI_MAX_SECTORS_PER_CMD) {
        return -EIO;
    }
    return ahci_run_command(d, ATA_READ_DMA_EXT, lba, count, (volatile uint8_t *)buffer,
                            count * BLOCK_SECTOR_SIZE, 0);
}

/**
 * The device's write: a polled `WRITE DMA EXT`, then `FLUSH CACHE EXT` for
 * durability.
 */
static int ahci_write(block_device_t *device, uint64_t lba, uint32_t count, const void *buffer)
{
    struct ahci_disk *d = (struct ahci_disk *)device->drvdata;
    int rc;

    if (count == 0 || lba > device->sector_count || count > device->sector_count - lba ||
        count > AHCI_MAX_SECTORS_PER_CMD) {
        return -EIO;
    }
    rc = ahci_run_command(d, ATA_WRITE_DMA_EXT, lba, count,
                          (volatile uint8_t *)(uintptr_t)buffer, count * BLOCK_SECTOR_SIZE, 1);
    if (rc != 0) {
        return rc;
    }
    return ahci_flush(d);
}

/**
 * Bring the controller up and publish every disk on it.
 *
 * Each port the scan kept is started and identified in port order; one that
 * fails is stopped and left, with a record, and the rest go on. The first
 * that answers is `block0`. The whole probe fails only when no port ends up
 * with a disk, so a machine whose second bay holds a dead drive still boots
 * from its first.
 */
int ahci_init(void)
{
    int i, published = 0;

    /* Leave the published device unset until bring-up succeeds, so a failed
     * probe fails closed regardless of caller (mirrors ata_init). */
    block0 = 0;
    ahci_base = 0;
    ahci_port = -1;
    ahci_ndisks = 0;
    ahci_vector = -1;
    ahci_detect_cache();
    if (ahci_discover() != 0) {
        klog(KLOG_INFO, "ahci", "none: %s", ahci_err);
        return -1;
    }
    if (ahci_hba_own() != 0 || ahci_quiesce_ports() != 0 || ahci_select_port() != 0) {
        klog(KLOG_ERR, "ahci", "unusable: %s", ahci_err);
        /* The registers behind the refusal, into the log the machine hands
         * out afterwards (/dev/bootlog): one failed boot then leaves enough
         * to decide between every cause the investigation named, instead of
         * costing another trip to the machine. */
        if (ahci_base) {
            ahci_dump();
        }
        return -1;
    }

    for (i = 0; i < ahci_ndisks; i++) {
        struct ahci_disk *d = &ahci_disks[i];

        if (ahci_port_start(d) != 0 || ahci_identify(d) != 0) {
            klog(KLOG_ERR, "ahci", "port %d: unusable: %s", d->port, ahci_err);
            (void)ahci_port_stop(d->port);
            continue;
        }
        d->dev.sector_size = BLOCK_SECTOR_SIZE;
        d->dev.sector_count = d->sectors;
        d->dev.max_sectors = BLOCK_REQ_MAX_SECTORS;
        d->dev.flags = BLOCK_FAULT_SAFE | BLOCK_POLLABLE;
        d->dev.read = ahci_read;
        d->dev.write = ahci_write;
        d->dev.submit = 0;
        d->dev.flush = 0;
        d->dev.backing = 0;
        d->dev.drvdata = d;
        d->active = 0;
        d->queue_head = d->queue_tail = 0;
        if (!block0) {
            block0 = &d->dev;
            ahci_port = d->port;
        }
        disk_register(&d->dev, "ahci", d->port, (volatile uint16_t *)ahci_idbuf);
        klog(KLOG_INFO, "ahci", "port %d: %d sectors, %d MiB", d->port, (uint32_t)d->sectors,
             (uint32_t)(d->sectors / 2048));
        published++;
    }
    if (published == 0) {
        klog(KLOG_ERR, "ahci", "unusable: no port's disk answered");
        ahci_dump();
        return -1;
    }
    return 0;
}

void ahci_dump(void)
{
    uint32_t cap, ghc, pi, vs;
    int port;

    if (!ahci_base) {
        console_printf("ahci: not initialized\n");
        return;
    }

    cap = ahci_read32(AHCI_CAP);
    ghc = ahci_read32(AHCI_GHC);
    pi = ahci_read32(AHCI_PI);
    vs = ahci_read32(AHCI_VS);
    console_printf("ahci: CAP=%X GHC=%X PI=%X VS=%X ports=%d active=%d\n", cap, ghc, pi, vs,
                  (int)((cap & AHCI_CAP_NP_MASK) + 1), ahci_port);
    /* CAP2 says whether a handoff exists, BOHC how it went, and PCS whether
     * the Intel port PHYs are enabled at all; each is one of the named causes
     * of a machine that finds no disk, so the dump that decides between them
     * has to carry all three. */
    console_printf("ahci: CAP2=%X BOHC=%X PCS=%X\n", ahci_read32(AHCI_CAP2), ahci_read32(AHCI_BOHC),
                  (unsigned)pci_read_config16(ahci_pci.bus, ahci_pci.device, ahci_pci.function,
                                              AHCI_INTEL_PCS));
    for (port = 0; port < ahci_ndisks; ++port) {
        console_printf("ahci: port %d dma cmdlist=%X fis=%X table=%X\n", ahci_disks[port].port,
                      (uint32_t)(uintptr_t)ahci_disks[port].cmdlist,
                      (uint32_t)(uintptr_t)ahci_disks[port].recv_fis,
                      (uint32_t)(uintptr_t)ahci_disks[port].cmdtable);
    }

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
        console_printf("ahci:  port %d: SSTS=%X (DET=%d) "
                      "SIG=%X TFD=%X CMD=%X CLB=%X\n",
                      port, ssts, (int)(ssts & AHCI_DET_MASK), sig, tfd, cmd, clb);
        console_printf("ahci:  port %d: SCTL=%X SERR=%X\n", port,
                      ahci_read32(ahci_port_off(port, AHCI_PxSCTL)),
                      ahci_read32(ahci_port_off(port, AHCI_PxSERR)));
    }
}

void ahci_selftest(void)
{
    static uint8_t buf[BLOCK_SECTOR_SIZE] __attribute__((aligned(2)));

    if (!block0) {
        console_printf("ahci: selftest: not initialized\n");
        return;
    }
    if (block0->read(block0, 0, 1, buf) != 0) {
        console_printf("ahci: selftest: lba 0 read failed\n");
        return;
    }
    console_printf("ahci: lba 0 signature %X %X\n", (uint32_t)buf[510], (uint32_t)buf[511]);
    if (buf[510] == 0x55 && buf[511] == 0xAA) {
        console_printf("ahci: selftest: MBR signature OK\n");
    } else {
        console_printf("ahci: selftest: MBR signature mismatch\n");
    }
}
