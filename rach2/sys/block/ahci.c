/*
 * ahci.c: the AHCI driver: the SATA disks on the first controller in AHCI
 * mode, polled at boot and on the fault path, driven by its message after.
 * Each port's state is under the port's own lock, never the big lock.
 */

#include "../arch/arch.h"
#include "../klog.h"
#include "ahci.h"
#include "diskcmd.h"
#include "disk.h"
#include "../udelay.h"
#include "block.h"

#include "../pci.h"
#include "../serial.h"
#include "../mm/kheap.h"
#include "../mm/paging.h"
#include "../irq.h"
#include "../lib/kirq.h"
#include "../lib/spinlock.h"
#include "../timer.h"
#include "../rtc.h"
#include "../cmdline.h"
#include "../latency.h"
#include "../panic.h"
#include "../proc/process.h"
#include "../proc/sched.h"
#include "../proc/smp.h"
#include "../proc/wait_queue.h"

#include "../lib/kerrno.h"
#include "../lib/kint.h"

/* AHCI prog-if within PCI class 0x01 subclass 0x06 (PCI_CLASS_MASS_STORAGE /
   PCI_SUBCLASS_SATA). A controller in IDE-compat or RAID mode reports a
   different prog-if and is skipped. */
#define AHCI_PCI_PROGIF 0x01

/* The ABAR (BAR5) MMIO span: the generic host control block (0x100 bytes)
   and 0x80 for each of up to 32 ports, 0x1100 in all; 0x2000 covers it. */
#define AHCI_ABAR_MAP_SIZE 0x2000

/* Generic host control registers, as offsets from the ABAR. */
#define AHCI_CAP  0x00 /* host capabilities */
#define AHCI_GHC  0x04 /* global host control */
#define AHCI_IS   0x08 /* interrupt status, one bit per port */
#define AHCI_PI   0x0C /* ports implemented (bitmask) */
#define AHCI_VS   0x10 /* version */
#define AHCI_CAP2 0x24 /* extended capabilities */
#define AHCI_BOHC 0x28 /* BIOS/OS handoff control and status */

#define AHCI_GHC_AE        0x80000000u /* AHCI enable */
#define AHCI_GHC_IE        0x00000002u /* global interrupt enable */
#define AHCI_CAP2_BOH      0x1u        /* BIOS/OS handoff supported */
#define AHCI_BOHC_BOS      0x1u        /* BIOS owned */
#define AHCI_BOHC_OOS      0x2u        /* OS ownership request */
#define AHCI_CAP_NP_MASK   0x1Fu       /* CAP.NP: number of ports minus one */
#define AHCI_CAP_S64A      0x80000000u /* CAP.S64A: 64-bit addressing */
#define AHCI_CAP_SSS       0x08000000u /* CAP.SSS: staggered spin-up */
/* CAP.ISS: the fastest link the controller supports, 1 for Gen1 (1.5 Gb/s),
   2 for Gen2 (3 Gb/s) and 3 for Gen3 (6 Gb/s); the other values are
   reserved. */
#define AHCI_CAP_ISS_SHIFT 20
#define AHCI_CAP_ISS_MASK  0xFu

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

/* PxCMD.ICC, the interface communication control: writing 1 requests that the
   link go active; the HBA clears the field when the transition is done. */
#define AHCI_PxCMD_ICC_MASK   0xF0000000u
#define AHCI_PxCMD_ICC_ACTIVE 0x10000000u

/* PCS, the Intel port control and status word in PCI configuration space:
   one enable bit per port. It is not an AHCI register, and Linux has a quirk
   of its own for it (`ahci_intel_pcs_quirk`). */
#define AHCI_INTEL_PCS 0x92

/* SSTS: DET (bits 0-3) == 3 is device present + PHY ready; SPD (bits 4-7) is
   the speed the link negotiated, numbered as CAP.ISS; IPM (bits 8-11) == 1 is
   the active power state. */
#define AHCI_DET_MASK    0xFu
#define AHCI_DET_PRESENT 0x3u
#define AHCI_IPM_ACTIVE  0x1u
#define AHCI_SPD_SHIFT   4
#define AHCI_SPD_MASK    0xFu

/* SCTL.DET: 1 requests a COMRESET, 0 lets the link come up again, and 4 takes
   the PHY offline. SCTL.SPD, at the bits of SSTS.SPD, is the fastest speed
   the link may negotiate, 0 for no limit. */
#define AHCI_SCTL_DET_MASK    0xFu
#define AHCI_SCTL_DET_INIT    0x1u
#define AHCI_SCTL_DET_NONE    0x0u
#define AHCI_SCTL_DET_OFFLINE 0x4u
#define AHCI_SCTL_SPD_MASK    0xF0u

/* SCTL.IPM with every link power state disabled: partial, slumber and
   DevSleep. Disabled at the host, they are also denied to the device's
   requests to enter them, so one write covers both the host's and the
   device's power management, HIPM and DIPM. */
#define AHCI_SCTL_IPM_MASK        0xF00u
#define AHCI_SCTL_IPM_DISABLE_ALL 0x700u

/* GHC.HR: the HBA-wide reset, which the HBA clears when it is done. */
#define AHCI_GHC_HR 0x00000001u

/* PxSIG, the signature the device leaves after a reset. A plain SATA disk
 * answers 0x00000101, which is the one this driver takes; an ATAPI device
 * answers 0xEB140101, a port multiplier 0x96690101, and an enclosure service
 * 0xC33C0101. */
#define AHCI_SIG_ATA   0x00000101u
#define AHCI_SIG_ATAPI 0xEB140101u

/* Every wait is a count of steps of a measured delay (`k_udelay`), not of
   bare register reads, since how long a read takes depends on the machine:
   ten million reads are a moment under QEMU and several seconds on real
   hardware. The bounds: one second for a handshake (the specification allows
   the host reset that long, and the others are shorter), and, polled
   every 10 us, thirty seconds for a command with interrupts on and five with
   them off. A healthy device answers a read in microseconds, but a write here
   ends with a flush, which waits for the medium, and an emulator's host under
   load or a disk spinning up can take seconds for that; thirty is the block
   request deadline's period and Linux's ATA command timeout. With interrupts
   off the timer does not tick, so the clock and every timer lose each tick of
   the wait, and the bound is the ATA driver's polled one. */
#define AHCI_WAIT_POLL_US     100u
#define AHCI_WAIT_STEPS       10000u
#define AHCI_CMD_POLL_US      10u
#define AHCI_CMD_STEPS        3000000u
#define AHCI_CMD_STEPS_MASKED 500000u

/* What follows a COMRESET is measured, not counted, since what it waits for
   happens on the wire and in the device, not at the processor's speed. The
   link is debounced as Linux 6.12.115 does it with `sata_deb_timing_normal`
   (drivers/ata/libata-sata.c:22): after the 200 ms that `sata_link_resume`
   gives the PHY before the first look (libata-sata.c:323), SStatus.DET is read
   every 5 ms and taken once it has held for 100 ms, so an empty port, whose DET
   stays 0, costs 300 ms. A DET of 1, a device seen that has not negotiated
   yet, is waited on for up to 2 s (`sata_link_debounce`, libata-sata.c:263).
   The device then has five seconds to finish its own reset and post its
   signature, polled every 100 us, which is generous for a disk and short
   enough that a port with something unresponsive on it does not hold up a
   boot. Every expiry is an ordinary answer: an empty port and a device that
   does not answer are both ports to move past. A DET that still changes after
   the 2 s is a failed reset, which is tried again at a lower speed, four tries
   in all, each after the deadline of the one before, 10, 10 and 35 s from its
   start, as Linux's `ata_eh_reset` does with `ata_eh_reset_timeouts`
   (libata-eh.c:81); the fourth's deadline, 5 s, is past its own debounce. */
#define AHCI_RESUME_DELAY_US 200000u
#define AHCI_DEB_POLL_US     5000u
#define AHCI_DEB_STEADY      20u  /* 100 ms of polls */
#define AHCI_DEB_LIMIT       400u /* 2 s of polls */
#define AHCI_RESET_TRIES     4u
#define AHCI_RESET_POLL_US   100u
#define AHCI_READY_STEPS     50000u

/* The DMA regions the HBA reads and writes, one set per disk. They are
   static so they land in the kernel image's .bss, below HARD_MEMORY_LIMIT,
   where their bytes are physically contiguous and below 4 GiB; the HBA is
   given `virt_to_phys` of each, both halves. The alignments are
   AHCI-mandated. */
#define AHCI_CMDLIST_SIZE  1024 /* 32 command headers of 32 bytes */
#define AHCI_RECV_FIS_SIZE 256
/* The command table: the 128-byte FIS area and then the PRDT, one 16-byte
 * entry per page of a heap buffer, so a request of BLOCK_REQ_MAX_SECTORS
 * that starts mid-page, 257 pieces, has room; 4352 is a multiple of the
 * 128-byte alignment the table needs. */
#define AHCI_CMDTABLE_SIZE 4352

/* How many disks one controller may publish. A port beyond the count is
   stopped and left, with a record. */
#define AHCI_DISKS 4

struct ahci_bounce;

/**
 * One disk on one port: its DMA areas, its capacity, and the device the
 * block layer sees, whose `drvdata` points back here.
 *
 * `lock` is the port's lock, as libata's `ap->lock` (drivers/ata/
 * libata-core.c:5567), taken with interrupts off by `submit`, the interrupt
 * and a polled command's claim. It covers the command slot and its tables,
 * `active`, `active_generation`, `flushing`, `polling`, the queue, the
 * bounce pages and the device's `unflushed`. Libata's is the host's lock,
 * one for all of a controller's ports; here each port has its own, since
 * ports share no state but the controller's interrupt status, whose bits
 * each port clears for itself alone. A completion is reported to the block
 * layer under it, as `ata_qc_complete` runs under `ap->lock`
 * (libata-core.c:4915-4927); the request pool's lock nests inside.
 */
struct ahci_disk {
    volatile uint8_t cmdlist[AHCI_CMDLIST_SIZE] __attribute__((aligned(1024)));
    volatile uint8_t recv_fis[AHCI_RECV_FIS_SIZE] __attribute__((aligned(256)));
    volatile uint8_t cmdtable[AHCI_CMDTABLE_SIZE] __attribute__((aligned(128)));
    struct spinlock lock;
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
    /* A polled command has the port: its caller claimed the slot under
     * `lock` and runs the command without it, and its status belongs to
     * that caller. The handler leaves the port alone while this is set,
     * since acknowledging would clear the error bits that the caller
     * reads, no queued request starts, and a second polled caller waits
     * for the claim to end (`ahci_claim`). */
    int polling;
    /* The claim's holder, the processor it claimed on and the thread it
     * runs, for a waiter that holds the big lock and cannot sleep
     * (`bkl_lend`); and the callers that sleep for the claim to end, which
     * its release wakes. */
    int polling_cpu;
    const struct process *polling_proc;
    wait_queue_t claim_wq;
    struct block_request *queue_head, *queue_tail;
    /* The pages a controller without 64-bit addressing moves a buffer's
     * pieces above 4 GiB through, or NULL on one with it. */
    struct ahci_bounce *bounce;
};

static struct ahci_disk ahci_disks[AHCI_DISKS];
static int ahci_ndisks;

/* The vector the controller's messages arrive on once `ahci_start` has
   switched the disks to the interrupt-driven path, -1 before and under
   `rach2.ahcipoll`, which keeps every transfer polled. */
static int ahci_vector = -1;

/* The areas a port is pointed at while it is reset, before the scan knows
   whether it has a disk. Nothing reads them: a port that is kept gets its
   own areas when it is started, and a port that is skipped is stopped. The
   ports are reset together, so each has a FIS area of its own for the HBA to
   write; they share the command list, which the HBA reads only once the
   command engine runs, and it stays off. */
static volatile uint8_t ahci_reset_cmdlist[AHCI_CMDLIST_SIZE] __attribute__((aligned(1024)));
static volatile uint8_t ahci_reset_fis[32][AHCI_RECV_FIS_SIZE] __attribute__((aligned(256)));

/* Task-file status bits (PxTFD low byte) and the fatal port-interrupt bits. A
   command fails on a device error (PxTFD ERR or DF) or on a fatal PxIS bit (the
   task-file error or a host-bus or interface fault PxTFD never reflects). */
#define AHCI_PxTFD_BSY   0x80u
#define AHCI_PxTFD_DRQ   0x08u
#define AHCI_PxTFD_DF    0x20u /* device fault, independent of ERR */
#define AHCI_PxTFD_ERR   0x01u
#define AHCI_PxIS_TFES   0x40000000u /* task file error status */
#define AHCI_PxIS_HBFS   0x20000000u /* host bus fatal error */
#define AHCI_PxIS_HBDS   0x10000000u /* host bus data error */
#define AHCI_PxIS_IFS    0x08000000u /* interface fatal error */
#define AHCI_PxIS_FATAL  (AHCI_PxIS_TFES | AHCI_PxIS_HBFS | AHCI_PxIS_HBDS | AHCI_PxIS_IFS)
/* The port interrupts the driven path enables: a D2H register FIS, which
   every command ends with, and the fatal ones. */
#define AHCI_PxIS_DHRS   0x00000001u
#define AHCI_PxIE_WANTED (AHCI_PxIS_DHRS | AHCI_PxIS_FATAL)
/* The status byte of the last D2H register FIS, which the HBA writes into
   the received-FIS area at 0x40 before it raises DHRS (Linux's
   RX_FIS_D2H_REG, drivers/ata/ahci.h:65); for a DMA or non-data command,
   which ends with that FIS, it is the status PxTFD holds. */
#define AHCI_RFIS_STATUS (0x40 + 2)

/* The H2D Register FIS and the ATA commands this driver issues. */
#define AHCI_FIS_H2D      0x27u
#define AHCI_FIS_H2D_C    0x80u /* C bit: this FIS has a command */
#define ATA_READ_DMA_EXT  0x25u
#define ATA_WRITE_DMA_EXT 0x35u
#define ATA_FLUSH_EXT     0xEAu /* FLUSH CACHE EXT, durability after a write */
#define ATA_IDENTIFY      0xECu
#define ATA_DEV_LBA       0x40u /* device register: LBA mode */

/* One PRDT entry transfers up to 4 MiB, so a polled command is held to that.
   The block layer asks for at most 2048 sectors, a megabyte, which is under
   it, and a larger request fails rather than having its byte count cut to
   the entry's 22 bits. */
#define AHCI_MAX_SECTORS_PER_CMD 8192u

/* The one command slot this driver uses, its 5-dword H2D FIS, and the PRDT
   offset (after CFIS 64 + ACMD 16 + reserved 48) inside the command table. */
#define AHCI_SLOT        0
#define AHCI_CFL_DWORDS  5
#define AHCI_CMD_W       (1u << 6)  /* command header: write direction */
#define AHCI_PRDTL_ONE   (1u << 16) /* command header: one PRDT entry */
#define AHCI_PRDT_OFFSET 0x80
/* The entries the command table has room for after the FIS: 264, so a
 * buffer from the heap, one entry per page, may span the 1 MiB request. */
#define AHCI_PRDT_MAX    ((AHCI_CMDTABLE_SIZE - AHCI_PRDT_OFFSET) / 16)
/* The most one request to the disk may move: the block layer's whole
 * limit, which the table describes page by page. */
#define AHCI_REQ_SECTORS BLOCK_REQ_MAX_SECTORS
#define AHCI_DBC_MASK    0x3FFFFFu /* PRDT byte count, bits 0-21 */

/* The first address a controller without CAP.S64A cannot reach. */
#define AHCI_FOUR_GIB 0x100000000ULL

/*
 * A controller whose CAP.S64A is clear takes only 32-bit addresses
 * (AHCI 1.3.1, 3.1.1), and a buffer of the heap may have pages above 4 GiB
 * on x86-64. Each piece of a buffer there moves through a page of the DMA
 * pool (`alloc_kernel_frame`), one page for each entry of the table, taken
 * when an entry first needs it and kept: a write's piece is copied in when
 * the command is built, and a read's is copied back when it is judged.
 * `to` is where a read's piece goes back, NULL for a write's piece and for an
 * entry the controller reaches directly, and `count` the entries of the
 * command in the slot.
 */
struct ahci_bounce {
    uint8_t *page[AHCI_PRDT_MAX];
    volatile uint8_t *to[AHCI_PRDT_MAX];
    uint32_t len[AHCI_PRDT_MAX];
    uint32_t count;
};

/* Whether the controller takes 64-bit addresses: CAP.S64A, unless the test
   switch `rach2.dma32` says to treat it as one that does not. */
static int ahci_dma64;

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

/* The controller's PCI identity, the ABAR as discovery mapped it (zero until
   then, and left set if bring-up later fails), and the port of the
   controller's first disk, `block0`'s once one is published, -1 until a port
   is selected. Every read and write through the ABAR is volatile at the
   access (`mmio_read32`). */
static pci_device_t ahci_pci;
static uintptr_t ahci_base;
static int ahci_port = -1;

/** The machine's register read, through the mapped ABAR. */
static uint32_t ahci_mmio_r32(uint32_t offset)
{
    return mmio_read32(ahci_base + offset);
}

/** The high half of a physical address, which the controller takes apart
 * from its low half. */
static uint32_t ahci_hi(uint64_t phys)
{
    return (uint32_t)(phys >> 32);
}

/** The machine's register write, through the mapped ABAR. */
static void ahci_mmio_w32(uint32_t offset, uint32_t value)
{
    mmio_write32(ahci_base + offset, value);
}

/* The machine's table, and the table in use, which `ahci_set_io` changes. */
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

/**
 * Poll the register at `offset` every 100 us until every bit of `mask` reads
 * 0. Returns 0, or -1 after a second, the handshake bound: a wedged HBA.
 */
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

/* Why the last bring-up stopped, which `ahci_init` logs, so that a failed
   probe says which step failed and not only that one did. */
static const char *ahci_err = "no SATA controller in AHCI mode on the bus";

/**
 * Find the first SATA controller in AHCI mode on the bus, map its ABAR, and
 * turn on its memory decoding. Returns 0, or -1 with `ahci_err` set. Bus
 * mastering is turned on later, once a port's DMA bases are programmed.
 *
 * The ABAR is mapped uncached wherever it is, in a mapping of its own in the
 * kernel's MMIO region (`map_mmio`), and never reached through the identity
 * mapping a low ABAR has, which is cacheable. When no controller is found, a
 * debug line gives what is at 0:31.2, where an Intel PCH has its SATA
 * controller: the controller is function 2 of a multi-function device, and a
 * scan reaches it only when function 0 says it has siblings, so "nothing
 * matched" alone cannot tell an absent controller from one the scan missed.
 */
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
        klog(KLOG_DEBUG, "ahci", "no match: 0:31.2 reads %X:%X class %X:%X prog-if %X",
             (unsigned)pci_read_config16(0, 31, 2, PCI_VENDOR_ID),
             (unsigned)pci_read_config16(0, 31, 2, PCI_DEVICE_ID),
             (unsigned)pci_read_config8(0, 31, 2, PCI_CLASS),
             (unsigned)pci_read_config8(0, 31, 2, PCI_SUBCLASS),
             (unsigned)pci_read_config8(0, 31, 2, PCI_PROG_IF));
        ahci_err = "no SATA controller in AHCI mode on the bus";
        return -1;
    }

    klog(KLOG_DEBUG, "ahci", "found: %X:%X at %d:%d.%d, prog-if %X, BAR5 %X",
         (unsigned)dev.vendor_id, (unsigned)dev.device_id, (int)dev.bus, (int)dev.device,
         (int)dev.function, (unsigned)dev.prog_if, (unsigned)dev.bar[5]);

    /* The ABAR is BAR5; a 64-bit BAR4 would take BAR5 as its high half. */
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
    mapped = map_mmio(abar, AHCI_ABAR_MAP_SIZE);
    if (mapped == 0) {
        ahci_err = "the ABAR window could not be mapped";
        return -1;
    }
    ahci_base = mapped;

    ahci_pci = dev;
    pci_enable_memory_space(&ahci_pci);

    return 0;
}

/**
 * Assert COMRESET on one port, with its reset areas programmed, its power up
 * and its FIS receive on, and its link limited to speed `spd`, an SCTL.SPD
 * value; `ahci_links_try` releases it.
 *
 * A warm reboot can leave a link where the port reports a device and the
 * device never becomes ready, as the Lenovo's does, and the HBA reset of
 * `ahci_hba_own` leaves every link down; the way back is to take the link
 * down and bring it up. DET=1 asserts COMRESET, which the specification
 * requires held for at least a millisecond, and DET=0 releases it, after
 * which the PHY negotiates again.
 *
 * The device answers a reset with a FIS that has its signature, and a FIS
 * arrives only when the port has somewhere to put it and is receiving, so
 * the reset areas are programmed and FIS receive turned on before the reset;
 * without them the task file never updates and every port looks like a
 * device that does not answer. The command engine stays off.
 *
 * The port's power comes up before its link. The HBA reset puts every port
 * at its defaults, and on a controller with staggered spin-up that leaves
 * PxCMD.SUD at 0, which the specification requires at 1 for a COMRESET to
 * act (AHCI 1.3.1, 10.4.2). QEMU's model resets ports with spin-up and power
 * already on, so only hardware shows this. Where the features are absent both
 * bits read as 1 and the writes change nothing, which is why Linux
 * (`ahci_power_up`) writes them unconditionally too; the ICC write then sets
 * the interface itself active.
 *
 * Link power management is disabled in the same write that asserts the
 * reset, and stays disabled. On the Lenovo, a warm reboot with the link power
 * states enabled left its drive invisible even to the firmware until power
 * was removed, and disabling them in the storage driver cured it. Linux's
 * quirk table marks the drive's family, "Samsung SSD 870*", scoped there to
 * ATI controllers, and its strongest policy, ATA_LPM_MAX_POWER, is this
 * value: SControl IPM = 7, every transition disabled (libata-sata.c). A
 * diagnostic that reads a sector an hour after boot must not find that the
 * link slept in between and the drive never woke.
 *
 * A new speed limit is written with the PHY offline, DET 4, as Linux's
 * `sata_link_hardreset` writes one (libata-sata.c:634-646), since the
 * specification does not say how a running link takes the change.
 */
static void ahci_port_comreset(int port, uint32_t spd)
{
    uint32_t sctl_off = ahci_port_off(port, AHCI_PxSCTL);
    uint32_t cmd_off = ahci_port_off(port, AHCI_PxCMD);
    uint32_t sctl = ahci_read32(sctl_off);

    ahci_write32(ahci_port_off(port, AHCI_PxCLB), (uint32_t)virt_to_phys(ahci_reset_cmdlist));
    ahci_write32(ahci_port_off(port, AHCI_PxCLBU), ahci_hi(virt_to_phys(ahci_reset_cmdlist)));
    ahci_write32(ahci_port_off(port, AHCI_PxFB), (uint32_t)virt_to_phys(ahci_reset_fis[port]));
    ahci_write32(ahci_port_off(port, AHCI_PxFBU), ahci_hi(virt_to_phys(ahci_reset_fis[port])));

    {
        uint32_t cmd = ahci_read32(cmd_off) | AHCI_PxCMD_SUD | AHCI_PxCMD_POD;

        ahci_write32(cmd_off, cmd);
        ahci_write32(cmd_off, (cmd & ~AHCI_PxCMD_ICC_MASK) | AHCI_PxCMD_ICC_ACTIVE);
    }

    ahci_write32(cmd_off, ahci_read32(cmd_off) | AHCI_PxCMD_FRE);

    sctl = (sctl & ~(AHCI_SCTL_DET_MASK | AHCI_SCTL_IPM_MASK)) | AHCI_SCTL_IPM_DISABLE_ALL;
    if (((sctl & AHCI_SCTL_SPD_MASK) >> AHCI_SPD_SHIFT) != spd) {
        ahci_write32(sctl_off, sctl | AHCI_SCTL_DET_OFFLINE);
        sctl = (sctl & ~AHCI_SCTL_SPD_MASK) | (spd << AHCI_SPD_SHIFT);
        ahci_write32(sctl_off, sctl | AHCI_SCTL_DET_OFFLINE);
    }
    ahci_write32(sctl_off, sctl | AHCI_SCTL_DET_INIT);
}

/**
 * Reset the SATA links of the ports in the mask `ports` together, each at the
 * speed limit `spd[port]`, and debounce them. Returns the mask of those whose
 * link came up, DET 3, with their PxSERR cleared; sets `*failed` to the mask
 * of those whose debounce failed and `*spent_us` to the time the delays took.
 *
 * This is one try of Linux's `sata_link_hardreset` (drivers/ata/
 * libata-sata.c:624, in 6.12.115), which `ahci_hardreset` runs
 * (libahci.c:1641): COMRESET held for at least a millisecond
 * (libata-sata.c:663), released, 200 ms for the PHY (`sata_link_resume`,
 * :323), then `sata_link_debounce` (:235) with the normal timings. A port is
 * settled once its DET has held for 100 ms, except that a DET of 1 is waited
 * on until the 2 s deadline. A DET that changes after the deadline fails the
 * port's debounce, where Linux returns -EPIPE (:275-279); the port is then
 * neither linked nor settled, and `ahci_links_reset` tries it again. Every
 * port is settled or failed within the deadline and a steady stretch past it.
 *
 * An empty port is not a failure: every implemented port is reset at
 * bring-up, and most machines have ports with nothing on them. Its DET stays
 * 0 and it is settled 300 ms after the release, which is what an empty port
 * costs Linux; a debug line gives each such port's status.
 */
static uint32_t ahci_links_try(uint32_t ports, const uint32_t *spd, uint32_t *failed,
                               uint32_t *spent_us)
{
    uint32_t last[32], since[32];
    uint32_t pending = ports, linked = 0, step;
    int p;

    *failed = 0;
    for (p = 0; p < 32; ++p) {
        if (ports & (1u << p)) {
            ahci_port_comreset(p, spd[p]);
        }
    }
    /* At least 1 ms with COMRESET asserted. */
    k_udelay(2000);
    for (p = 0; p < 32; ++p) {
        if (ports & (1u << p)) {
            uint32_t sctl_off = ahci_port_off(p, AHCI_PxSCTL);

            ahci_write32(sctl_off,
                         (ahci_read32(sctl_off) & ~AHCI_SCTL_DET_MASK) | AHCI_SCTL_DET_NONE);
        }
    }
    k_udelay(AHCI_RESUME_DELAY_US);

    for (p = 0; p < 32; ++p) {
        if (ports & (1u << p)) {
            last[p] = ahci_read32(ahci_port_off(p, AHCI_PxSSTS)) & AHCI_DET_MASK;
            since[p] = 0;
        }
    }
    for (step = 1; pending != 0; ++step) {
        k_udelay(AHCI_DEB_POLL_US);
        for (p = 0; p < 32; ++p) {
            uint32_t ssts, cur;

            if (!(pending & (1u << p))) {
                continue;
            }
            ssts = ahci_read32(ahci_port_off(p, AHCI_PxSSTS));
            cur = ssts & AHCI_DET_MASK;
            if (cur == last[p]) {
                if ((cur == 1 && step < AHCI_DEB_LIMIT) || step - since[p] <= AHCI_DEB_STEADY) {
                    continue;
                }
            } else {
                last[p] = cur;
                since[p] = step;
                if (step > AHCI_DEB_LIMIT) {
                    pending &= ~(1u << p);
                    *failed |= 1u << p;
                }
                continue;
            }
            pending &= ~(1u << p);
            if (cur == AHCI_DET_PRESENT) {
                uint32_t serr_off = ahci_port_off(p, AHCI_PxSERR);

                /* PxSERR is write-1-to-clear: clears the link-up's errors. */
                ahci_write32(serr_off, ahci_read32(serr_off));
                linked |= 1u << p;
            } else {
                klog(KLOG_DEBUG, "ahci",
                     "port %d: no link after %d ms; SStatus 0x%X, DET %d (0 is nothing attached, "
                     "1 is a device that did not negotiate)",
                     p, (int)((AHCI_RESUME_DELAY_US + step * AHCI_DEB_POLL_US) / 1000u), ssts,
                     (int)cur);
            }
        }
    }
    *spent_us = 2000u + AHCI_RESUME_DELAY_US + (step - 1u) * AHCI_DEB_POLL_US;
    return linked;
}

/**
 * Lower the speed limit `*limit` of a port whose link failed its debounce, a
 * mask in which bit N-1 allows SCTL.SPD N. Returns the new limit's SCTL.SPD,
 * or 0 when the limit is already at its lowest and is left as it is.
 *
 * This is Linux's `sata_down_spd_limit` (libata-sata.c:540): the fastest
 * speed allowed is dropped, and so is every speed from the one the link last
 * negotiated up, which SStatus gives while DET reads 3. Linux starts a limit
 * that SControl does not set at UINT_MAX, and dropping its top bit with no
 * speed known writes a reserved SPD, 15 (dev-log/84-locks/upstream.md);
 * `ahci_links_reset` starts it at the controller's speeds, CAP.ISS, so the
 * first such step drops the fastest of those.
 */
static uint32_t ahci_link_slow(int port, uint32_t *limit)
{
    uint32_t ssts = ahci_read32(ahci_port_off(port, AHCI_PxSSTS));
    uint32_t mask = *limit, cur = 0, spd = 0;
    static const char *const speeds[] = {"1.5 Gb/s", "3 Gb/s", "6 Gb/s"};

    if ((ssts & AHCI_DET_MASK) == AHCI_DET_PRESENT) {
        cur = (ssts >> AHCI_SPD_SHIFT) & AHCI_SPD_MASK;
    }
    if (mask <= 1u) {
        return 0;
    }
    while ((mask >> spd) > 1u) {
        ++spd;
    }
    mask &= ~(1u << spd);
    if (cur > 1u) {
        mask &= (1u << (cur - 1u)) - 1u;
    }
    if (mask == 0) {
        return 0;
    }
    *limit = mask;
    for (spd = 0; mask != 0; mask >>= 1) {
        ++spd;
    }
    klog(KLOG_WARN, "ahci", "port %d: limiting the link to %s", port,
         spd <= 3u ? speeds[spd - 1u] : "a lower speed");
    return spd;
}

/* How long each failed try of a link reset lasts before the next starts, from
   its own start, in ms: Linux's `ata_eh_reset_timeouts` (libata-eh.c:81). */
static const uint32_t ahci_reset_wait_ms[AHCI_RESET_TRIES - 1u] = {10000u, 10000u, 35000u};

/**
 * Reset the SATA links of the ports in the mask `ports` together and debounce
 * them, trying a link whose debounce fails again at a lower speed. Returns
 * the mask of those whose link came up, DET 3, with their PxSERR cleared.
 *
 * This is the hardreset loop of Linux's `ata_eh_reset` (libata-eh.c:2775 and
 * 2942-3029), for the ports together. A port whose DET still changes after
 * the debounce's deadline, a link that does not hold, waits out the try's
 * deadline, `ahci_reset_wait_ms`, has its speed lowered (`ahci_link_slow`)
 * and is reset again, with the other failed ports, four tries in all; a link
 * that has not held by then is skipped with a warning, as Linux gives up on
 * it. A port whose link held, or that settled with no link, is done after its
 * first try; most links hold at once, and an empty port costs only that.
 */
static uint32_t ahci_links_reset(uint32_t ports)
{
    uint32_t spd[32], limit[32];
    uint32_t linked = 0, failed, spent_us, iss, try;
    int p;

    iss = (ahci_read32(AHCI_CAP) >> AHCI_CAP_ISS_SHIFT) & AHCI_CAP_ISS_MASK;
    if (iss == 0) {
        iss = 3; /* reserved; the fastest SATA speed there is */
    }
    for (p = 0; p < 32; ++p) {
        spd[p] = limit[p] = 0;
        if (!(ports & (1u << p))) {
            continue;
        }
        spd[p] =
            (ahci_read32(ahci_port_off(p, AHCI_PxSCTL)) & AHCI_SCTL_SPD_MASK) >> AHCI_SPD_SHIFT;
        limit[p] = (1u << iss) - 1u;
        if (spd[p] != 0) {
            limit[p] &= (1u << spd[p]) - 1u;
        }
    }

    pci_enable_bus_master(&ahci_pci);
    for (try = 0;; ++try) {
        linked |= ahci_links_try(ports, spd, &failed, &spent_us);
        if (failed == 0) {
            break;
        }
        if (try + 1u >= AHCI_RESET_TRIES) {
            for (p = 0; p < 32; ++p) {
                if (failed & (1u << p)) {
                    klog(KLOG_WARN, "ahci", "port %d: the link did not hold in %d resets; skipped",
                         p, (int)AHCI_RESET_TRIES);
                }
            }
            break;
        }
        if (spent_us < ahci_reset_wait_ms[try] * 1000u) {
            k_udelay(ahci_reset_wait_ms[try] * 1000u - spent_us);
        }
        for (p = 0; p < 32; ++p) {
            if (failed & (1u << p)) {
                uint32_t lower = ahci_link_slow(p, &limit[p]);

                if (lower != 0) {
                    spd[p] = lower;
                }
            }
        }
        ports = failed;
    }
    return linked;
}

/**
 * Take the controller from the firmware, in AHCI mode, and reset it. Returns
 * 0, or -1 with `ahci_err` set when the reset does not finish in a second.
 *
 * The reset (GHC.HR) puts the registers in a known state, which matters on a
 * machine whose firmware has been using the controller. It also leaves every
 * SATA link down, with a port that reports a device that is not ready (on
 * QEMU's ich9-ahci the port's TFD loses DRDY, 0x50 becoming 0x130), which the
 * COMRESET of each port (`ahci_links_reset`) brings back.
 *
 * The BIOS/OS handoff is requested when the controller has it (QEMU's does
 * not), and a request the firmware does not answer within a second is a
 * warning and not a failure: Linux never performs the handoff (BOHC appears
 * in its AHCI driver only as a capability bit's name), so firmware with a
 * broken handoff semaphore would fail only this kernel, on a controller Linux
 * takes. AHCI mode is set before the reset, since the specification defines
 * the registers, GHC.HR among them, only in AHCI mode. The controller's
 * interrupts stay off through bring-up, which polls; `ahci_start` turns them
 * on.
 *
 * Intel's Lynx Point and Wildcat Point controllers (device IDs 0x8Cxx and
 * 0x9Cxx), the families the board profiles cover, also have an enable bit
 * for each port in PCI configuration space (PCS, word 0x92), outside what
 * GHC.HR resets and what the AHCI specification names; a port whose bit is
 * clear has no PHY, and no COMRESET can wake it. Every implemented port's bit
 * is set, as Linux sets them after every controller reset
 * (`ahci_intel_pcs_quirk`, which its PCI table applies to the Lenovo's
 * controller, 8086:9c03). Newer Intel platforms moved the register, so the
 * write is held to those two families.
 */
static int ahci_hba_own(void)
{
    if (ahci_read32(AHCI_CAP2) & AHCI_CAP2_BOH) {
        ahci_write32(AHCI_BOHC, ahci_read32(AHCI_BOHC) | AHCI_BOHC_OOS);
        if (ahci_wait_clear(AHCI_BOHC, AHCI_BOHC_BOS) != 0) {
            klog(KLOG_WARN, "ahci", "handoff: the firmware did not answer; continuing");
        }
    }

    ahci_write32(AHCI_GHC, ahci_read32(AHCI_GHC) | AHCI_GHC_AE);
    ahci_write32(AHCI_GHC, ahci_read32(AHCI_GHC) | AHCI_GHC_HR);
    /* The HBA clears HR when it is done; the specification allows a second. */
    if (ahci_wait_clear(AHCI_GHC, AHCI_GHC_HR) != 0) {
        ahci_err = "the host reset never completed";
        return -1;
    }
    /* The reset clears AE. */
    ahci_write32(AHCI_GHC, (ahci_read32(AHCI_GHC) | AHCI_GHC_AE) & ~AHCI_GHC_IE);

    if (ahci_pci.vendor_id == 0x8086 &&
        ((ahci_pci.device_id & 0xFF00u) == 0x8C00u || (ahci_pci.device_id & 0xFF00u) == 0x9C00u)) {
        uint16_t pcs =
            pci_read_config16(ahci_pci.bus, ahci_pci.device, ahci_pci.function, AHCI_INTEL_PCS);
        uint16_t ports = (uint16_t)(ahci_read32(AHCI_PI) & 0xFFFFu);

        if ((pcs & ports) != ports) {
            pci_write_config16(ahci_pci.bus, ahci_pci.device, ahci_pci.function, AHCI_INTEL_PCS,
                               (uint16_t)(pcs | ports));
            klog(KLOG_DEBUG, "ahci", "pcs: enabled ports %X (was %X)", (unsigned)ports,
                 (unsigned)pcs);
        }
    }
    return 0;
}

/**
 * Stop a port's command engine and FIS receive, and wait for its PxCI and
 * PxSACT to clear. Returns 0, or -1 when one of them does not in a second.
 */
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

/**
 * Mask every implemented port's interrupts, stop it, and clear its error
 * and interrupt status. Returns 0, or -1 with `ahci_err` set when a port
 * does not stop.
 */
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
 * Wait for a port's device to finish its reset. Returns 0 when it has, and
 * -1 after five seconds.
 *
 * After a COMRESET the device runs its own reset and then posts a D2H FIS
 * with its signature; until that has happened the task file reports busy
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
 * Select the implemented ports carrying SATA disks, up to `AHCI_DISKS`.
 *
 * Every implemented port is COMRESET first, because a link that a warm reboot
 * left wedged reports a device that never becomes ready, and the scan would
 * otherwise select it and then fail on the identify. The ports are reset
 * together, unless the controller has staggered spin-up (CAP.SSS), when
 * Linux scans its ports one at a time, each port's device ready before the
 * next port is reset, so that their drives do not spin up at once
 * (`ATA_HOST_PARALLEL_SCAN`, drivers/ata/ahci.c:2131, and `async_port_probe`,
 * libata-core.c:5974). A port with no link, nothing there or a device that
 * did not negotiate one, is skipped, since most machines have some; the
 * reset itself logs a debug line for it. A link is up when DET reads 3,
 * whatever IPM says, as Linux's
 * `ata_phys_link_online` tests: a link that negotiated and then went to a
 * low-power interface state is live, and the ICC write in the reset brings it
 * back.
 *
 * The signature decides what the port has, and only a disk is taken. An
 * optical drive answers the ATAPI signature and would fail every command this
 * driver knows; a port multiplier and an enclosure service answer their own.
 * Taking the one signature this driver drives, instead of excluding the ones
 * it knows of, means an unfamiliar device is skipped rather than driven as if
 * it were a disk.
 *
 * A port that is skipped is stopped again before the scan moves on. The reset
 * had to enable that port's FIS receive engine, and a port left running with
 * no area of its own programmed would write wherever its base registers
 * point. The Lenovo has a SATA disk and a bay that may hold an optical drive,
 * which is two ports.
 *
 * Every port with a disk is kept, up to `AHCI_DISKS`, in port order; the
 * first is `ahci_port`. The function fails only when no port has a disk.
 */
static int ahci_select_port(void)
{
    uint32_t pi = ahci_read32(AHCI_PI), linked = 0;
    int serial = (ahci_read32(AHCI_CAP) & AHCI_CAP_SSS) != 0;
    int p;

    ahci_ndisks = 0;
    ahci_port = -1;
    if (!serial) {
        linked = ahci_links_reset(pi);
    }
    for (p = 0; p < 32; ++p) {
        uint32_t ssts, sig;

        if (!(pi & (1u << p))) {
            continue;
        }
        if (serial) {
            linked |= ahci_links_reset(1u << p);
        }
        if (!(linked & (1u << p))) {
            (void)ahci_port_stop(p);
            continue;
        }
        ssts = ahci_read32(ahci_port_off(p, AHCI_PxSSTS));
        if ((ssts & AHCI_DET_MASK) != AHCI_DET_PRESENT) {
            (void)ahci_port_stop(p);
            continue;
        }
        /* PxSIG reads all ones until the device's first FIS. */
        if (ahci_port_ready(p) != 0) {
            klog(KLOG_WARN, "ahci", "port %d: the device never became ready; skipped", p);
            (void)ahci_port_stop(p);
            continue;
        }
        sig = ahci_read32(ahci_port_off(p, AHCI_PxSIG));
        if (sig != AHCI_SIG_ATA) {
            klog(KLOG_DEBUG, "ahci", "port %d: signature %X is not a disk; skipped", p, sig);
            (void)ahci_port_stop(p);
            continue;
        }
        if (ahci_ndisks >= AHCI_DISKS) {
            klog(KLOG_WARN, "ahci", "port %d: a disk past the %d this driver publishes; skipped", p,
                 AHCI_DISKS);
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

/**
 * Stop `d`'s port, program its command list and FIS area, clear its error
 * and interrupt status, turn on bus mastering, and start the port again.
 * Returns 0, or -1 with `ahci_err` set when the port does not stop.
 */
static int ahci_port_start(struct ahci_disk *d)
{
    int port = d->port;
    uint32_t cmd_off = ahci_port_off(port, AHCI_PxCMD);

    if (ahci_port_stop(port) != 0) {
        ahci_err = "the selected port would not stop";
        return -1;
    }

    ahci_write32(ahci_port_off(port, AHCI_PxCLB), (uint32_t)virt_to_phys(d->cmdlist));
    ahci_write32(ahci_port_off(port, AHCI_PxCLBU), ahci_hi(virt_to_phys(d->cmdlist)));
    ahci_write32(ahci_port_off(port, AHCI_PxFB), (uint32_t)virt_to_phys(d->recv_fis));
    ahci_write32(ahci_port_off(port, AHCI_PxFBU), ahci_hi(virt_to_phys(d->recv_fis)));

    /* PxSERR and PxIS are write-1-to-clear. */
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

/**
 * Recover the port after a failed or timed-out command. Returns `-EIO`.
 *
 * AHCI 6.2.2 error recovery: a fatal error stops the command engine and may
 * leave a command outstanding, so without recovery the next command would issue
 * on a stopped engine and wedge the port until a fresh `ahci_init`. Clearing
 * `PxCMD.ST` resets `PxCI`; once `CR` is clear the latched errors are cleared
 * and the engine restarted, leaving the port able to serve the next command.
 * An engine that does not stop is a panic, with interrupts masked, before a
 * request or a polled caller can release its DMA buffer: the controller may
 * still use the buffer and the command table, so neither may be reused. Every
 * wait is bounded MMIO, so it is safe with interrupts off.
 */
static int ahci_io_error(struct ahci_disk *d)
{
    uint32_t cmd_off = ahci_port_off(d->port, AHCI_PxCMD);
    uint32_t flags = kirq_save();

    ahci_write32(cmd_off, ahci_read32(cmd_off) & ~AHCI_PxCMD_ST);
    if (ahci_wait_clear(cmd_off, AHCI_PxCMD_CR) != 0) {
        k_panic("ahci: recovery: command engine did not stop; DMA buffer still owned");
    }
    ahci_write32(ahci_port_off(d->port, AHCI_PxSERR), 0xFFFFFFFFu);
    ahci_write32(ahci_port_off(d->port, AHCI_PxIS), 0xFFFFFFFFu);
    ahci_write32(cmd_off, ahci_read32(cmd_off) | AHCI_PxCMD_ST);
    kirq_restore(flags);
    return -EIO;
}

/**
 * The address entry `n` of the table gives the controller for the `len`
 * bytes at `va`, whose physical address is `*phys`: that address itself,
 * or, on a controller without 64-bit addressing and for a piece that ends
 * above 4 GiB, the address of the entry's page of the DMA pool, with a
 * write's bytes copied into it. Returns 0, or -1 when no page could be had.
 */
static int ahci_piece(struct ahci_disk *d, uint32_t n, volatile uint8_t *va, uint32_t len,
                      int write, uint64_t *phys)
{
    struct ahci_bounce *b = d->bounce;
    uint32_t i;

    if (ahci_dma64 || *phys + len <= AHCI_FOUR_GIB) {
        if (b) {
            b->to[n] = 0;
        }
        return 0;
    }
    if (!b || (!b->page[n] && !(b->page[n] = (uint8_t *)alloc_kernel_frame()))) {
        return -1;
    }
    if (write) {
        for (i = 0; i < len; i++) {
            b->page[n][i] = va[i];
        }
    }
    b->to[n] = write ? 0 : va;
    b->len[n] = len;
    *phys = virt_to_phys(b->page[n]);
    return 0;
}

/** Copy a read's pieces back from the pages they moved through. */
static void ahci_unbounce(struct ahci_disk *d)
{
    struct ahci_bounce *b = d->bounce;
    uint32_t n, i;

    for (n = 0; b && n < b->count; n++) {
        for (i = 0; b->to[n] && i < b->len[n]; i++) {
            b->to[n][i] = b->page[n][i];
        }
    }
}

/**
 * Build the command in `d`'s slot: the command-list header, a Register H2D
 * FIS, and the PRDT over the disk's DMA regions, synced for the HBA.
 *
 * A `buf` below HARD_MEMORY_LIMIT, which only i686's identity window has, is
 * physically contiguous and goes in one PRDT entry; any other is a buffer
 * whose pages may lie anywhere, and gets an entry per page with the page's
 * physical address, both halves, up to the AHCI_PRDT_MAX entries of the
 * table, a piece above 4 GiB moving through the DMA pool on a controller
 * without 64-bit addressing (`ahci_piece`). A read into page frames has
 * its table from `ahci_frames_table` after this.
 *
 * Returns 0, or -1 when the disk policy does not allow the command or the
 * table cannot describe the buffer. Every command this driver sends is built
 * here, so this is the one place the policy is applied.
 */
static int ahci_build_command(struct ahci_disk *d, uint8_t command, uint64_t lba, uint32_t count,
                              volatile uint8_t *buf, uint32_t bytes, int write)
{
    volatile ahci_cmd_header_t *hdr = (volatile ahci_cmd_header_t *)d->cmdlist;
    volatile ahci_h2d_fis_t *fis = (volatile ahci_h2d_fis_t *)d->cmdtable;
    volatile ahci_prdt_t *prdt = (volatile ahci_prdt_t *)(d->cmdtable + AHCI_PRDT_OFFSET);
    uint32_t prdtl = 0;

    if (!diskcmd_allowed(command)) {
        klog(KLOG_WARN, "ahci", "refused: command 0x%X: %s", (unsigned)command,
             diskcmd_policy_banner);
        return -1;
    }

    ahci_zero(d->cmdtable, AHCI_CMDTABLE_SIZE);
    if (d->bounce) {
        d->bounce->count = 0;
    }

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
        prdt->dba = (uint32_t)virt_to_phys(buf);
        prdt->dbau = 0;
        prdt->reserved = 0;
        prdt->dbc = (bytes - 1) & AHCI_DBC_MASK;
        prdtl = AHCI_PRDTL_ONE;
    } else if (bytes > 0) {
        uint32_t off = 0, n = 0;

        while (off < bytes) {
            uintptr_t va = (uintptr_t)buf + off;
            uint64_t phys = paging_virt_to_phys(va);
            uint32_t chunk = PAGE_SIZE - (uint32_t)(va & (PAGE_SIZE - 1));

            if (chunk > bytes - off) {
                chunk = bytes - off;
            }
            if (phys == 0 || n >= AHCI_PRDT_MAX ||
                ahci_piece(d, n, (volatile uint8_t *)va, chunk, write, &phys) != 0) {
                klog(KLOG_WARN, "ahci", "refused: a buffer the command table cannot describe");
                return -1;
            }
            prdt[n].dba = (uint32_t)phys;
            prdt[n].dbau = ahci_hi(phys);
            prdt[n].reserved = 0;
            prdt[n].dbc = (chunk - 1) & AHCI_DBC_MASK;
            n++;
            off += chunk;
        }
        if (d->bounce) {
            d->bounce->count = n;
        }
        prdtl = n << 16;
    }

    hdr->dw0 = AHCI_CFL_DWORDS | (write ? AHCI_CMD_W : 0u) | prdtl;
    hdr->prdbc = 0;
    hdr->ctba = (uint32_t)virt_to_phys(d->cmdtable);
    hdr->ctbau = ahci_hi(virt_to_phys(d->cmdtable));
    return 0;
}

/**
 * Judge the command that has left the slot from its port's interrupt status
 * `is` and its task-file status `status`: 0 when it completed and moved
 * `bytes`, `-EIO` after the port's recovery otherwise. The HBA counts the
 * bytes it moved into `prdbc`; a completion that moved fewer than requested
 * without raising an error would otherwise pass, and a short write that
 * pc_sync then marks clean is silent data loss. A fatal PxIS error can
 * clear PxCI without setting the status's ERR, so both are tested. A read's
 * bytes in a bounce page are copied back to `buf`. Neither the descriptors
 * nor the buffer need the cache written back or evicted: x86 bus-master DMA
 * snoops the caches, and Linux's DMA sync does nothing for a coherent device
 * (kernel/dma/direct.h:56-75, include/linux/dma-map-ops.h:244-247).
 */
static int ahci_judge(struct ahci_disk *d, uint32_t is, uint32_t status, uint32_t bytes, int write)
{
    volatile ahci_cmd_header_t *hdr = (volatile ahci_cmd_header_t *)d->cmdlist;

    if ((is & AHCI_PxIS_FATAL) || (status & (AHCI_PxTFD_ERR | AHCI_PxTFD_DF))) {
        klog(KLOG_WARN, "ahci", "port %d: a command failed: the device reported an error", d->port);
        return ahci_io_error(d);
    }
    if (bytes > 0 && hdr->prdbc != bytes) {
        klog(KLOG_WARN, "ahci", "port %d: a command failed: it moved %d of %d bytes", d->port,
             (unsigned)hdr->prdbc, (unsigned)bytes);
        return ahci_io_error(d);
    }
    if (!write && bytes > 0) {
        ahci_unbounce(d);
    }
    return 0;
}

/** Judge the command that has left the slot of a polled caller, from the
 * port's registers, as `ahci_judge` does. */
static int ahci_judge_polled(struct ahci_disk *d, uint32_t bytes, int write)
{
    uint32_t is = ahci_read32(ahci_port_off(d->port, AHCI_PxIS));

    return ahci_judge(d, is, ahci_read32(ahci_port_off(d->port, AHCI_PxTFD)), bytes, write);
}

/**
 * Clear `d`'s port interrupt status before another command is issued, and
 * its bit alone of the controller's, so another port's completion remains
 * pending.
 */
static void ahci_acknowledge(struct ahci_disk *d)
{
    uint32_t is_off = ahci_port_off(d->port, AHCI_PxIS);

    ahci_write32(is_off, ahci_read32(is_off));
    ahci_write32(AHCI_IS, 1u << d->port);
}

static void ahci_drain(struct ahci_disk *d);
static void ahci_start_next(struct ahci_disk *d);
static int ahci_flush(struct ahci_disk *d);

/** The polled wait's bound in steps: `AHCI_CMD_STEPS` with interrupts on, and
 * `AHCI_CMD_STEPS_MASKED` with them off, as the budgets above say. */
static uint32_t ahci_cmd_steps(void)
{
    return kirq_enabled() ? AHCI_CMD_STEPS : AHCI_CMD_STEPS_MASKED;
}

/**
 * One step of a polled wait: with interrupts off, answer the requests other
 * processors left this one (`smp_service`), as a spinlock's waiter does
 * (lib/spinlock.c), since a processor dropping a translation everywhere
 * waits for this one's answer and no interrupt brings it; then the delay.
 * Any context, the port's lock held or not.
 */
static void ahci_poll_step(void)
{
    if (!kirq_enabled()) {
        smp_service();
    }
    k_udelay(AHCI_CMD_POLL_US);
}

/**
 * Whether a wait for another caller's claim may sleep: interrupts on, inside
 * a system call, and the scheduler running, as the ATA driver's waits ask
 * (`ata_can_sleep`). Such a caller sleeps for the port in the queued path
 * too, so it holds nothing that a sleep may not let go.
 */
static int ahci_can_sleep(void)
{
    return kirq_enabled() && proc_in_syscall() && sched_is_enabled();
}

/* Microseconds in a timer tick, which converts a polled bound in steps to
   a sleep's deadline in ticks and back. */
#define AHCI_TICK_US (1000000u / TICK_HZ)

/** A sleep for the port: the port, and the tick at which the caller's bound
 * ends. */
struct ahci_claim_wait {
    struct ahci_disk *d;
    uint64_t due;
};

/** The condition a caller sleeping for the port waits on: no claim, or the
 * end of its bound. */
static int ahci_unclaimed(void *arg)
{
    const struct ahci_claim_wait *w = (const struct ahci_claim_wait *)arg;

    return !w->d->polling || rtc_ticks_now() >= w->due;
}

/**
 * Sleep once for another caller's claim on `d`, within the bound the
 * caller's command shares with its waits, as libata's internal command
 * waits with `wait_for_completion_timeout` (drivers/ata/libata-core.c:1616).
 * The first sleep turns `*steps` into a deadline in ticks, `*due`, which
 * every later sleep of the same command keeps: a claim another caller takes
 * before this one runs sends it to sleep again with only what is left. After
 * each sleep `*steps` is cut to what is left, so the command that follows
 * runs within the same bound. The process's timer is armed for the deadline
 * and restored after. Returns 0, or -1 when the bound has run out.
 * Process context, with the port's lock let go. May sleep.
 */
static int ahci_claim_sleep(struct ahci_disk *d, uint64_t *due, uint32_t *steps)
{
    struct ahci_claim_wait w;
    uint64_t now = rtc_ticks_now();
    uint64_t saved;
    uint32_t ticks = (*steps * AHCI_CMD_POLL_US + AHCI_TICK_US - 1u) / AHCI_TICK_US;

    if (*due == 0) {
        /* A tick more, so that a sleep begun late in a tick is never short. */
        *due = now + ticks + 1u;
    }
    if (now >= *due) {
        return -1;
    }
    w.d = d;
    w.due = *due;
    saved = current_proc->wakeup_tick;
    current_proc->wakeup_tick = *due;
    (void)wq_wait_event_nointr(&d->claim_wq, ahci_unclaimed, &w);
    current_proc->wakeup_tick = saved;
    current_proc->disk_waits++;

    now = rtc_ticks_now();
    if (now >= *due) {
        return -1;
    }
    if (*due - now < ticks) {
        *steps = (uint32_t)(*due - now) * (AHCI_TICK_US / AHCI_CMD_POLL_US);
    }
    return 0;
}

/**
 * One step of the wait for the claim that thread `owner` took on processor
 * `cpu`, by a caller that cannot sleep. When the caller holds the big lock
 * and an interrupt that `owner` took waits for it, the owner cannot go on
 * until the lock is free, and the caller, which cannot let the lock go,
 * would wait out its bound for nothing; it lends the lock to that processor
 * alone (`bkl_lend`), which gives it back at the interrupt's end. Then the
 * poll step.
 */
static void ahci_claim_step(int cpu, const struct process *owner)
{
    uint32_t flags = kirq_save();

    (void)bkl_lend(cpu, owner);
    kirq_restore(flags);
    ahci_poll_step();
}

/**
 * Claim `d`'s slot for polled command `command`, as libata's internal
 * command takes the port from its queue under `ap->lock` and runs with the
 * lock let go (drivers/ata/libata-core.c:1546-1597, 1663). Under the port's
 * lock, a claim another polled caller holds is waited out with the lock let
 * go, the active queued command is finished by polling and its request
 * completed (`ahci_drain`), and `polling` is set. The lock is then let go:
 * the claim keeps the interrupt off the port and the queue stopped until
 * `ahci_release`, so the command runs with interrupts as the caller had
 * them.
 *
 * The wait takes from `*steps`, the caller's bound for the whole command,
 * as libata bounds an internal command by one timeout (libata-core.c:1616).
 * A caller that can sleep (`ahci_can_sleep`) sleeps for another caller's
 * claim on the port's queue, which the claim's release wakes, until a
 * deadline that its retries share (`ahci_claim_sleep`), and the switch lets
 * the big lock go, as libata gives back the port's lock and the error
 * handler's mutex before its internal command waits (libata-core.c:1596,
 * 1607-1616): the holder may need the lock to go on, for an interrupt it
 * took. Any other caller spins, lending the big lock to the holder's
 * processor when it holds it (`ahci_claim_step`), a step at a time: a
 * caller with interrupts off, whose bound is five seconds, cannot spin for
 * the thirty seconds and more of a holder's with them on. Returns 0 with
 * the port claimed and `*steps` what is left, or `-EIO`, logged, with
 * nothing claimed and the port untouched, since its state is the holder's.
 */
static int ahci_claim(struct ahci_disk *d, uint8_t command, uint32_t *steps)
{
    uint32_t flags = spin_lock_irqsave(&d->lock);
    uint64_t due = 0;

    while (d->polling) {
        int cpu = d->polling_cpu;
        const struct process *owner = d->polling_proc;
        int expired;

        spin_unlock_irqrestore(&d->lock, flags);
        if (ahci_can_sleep()) {
            expired = ahci_claim_sleep(d, &due, steps) != 0;
        } else {
            expired = --*steps == 0;
            if (!expired) {
                ahci_claim_step(cpu, owner);
            }
        }
        if (expired) {
            klog(KLOG_WARN, "ahci", "port %d: command 0x%X failed: %s", d->port, (unsigned)command,
                 "another polled command held the port");
            return -EIO;
        }
        flags = spin_lock_irqsave(&d->lock);
    }
    ahci_drain(d);
    d->polling = 1;
    d->polling_cpu = (int)arch_cpu_id();
    d->polling_proc = current_proc;
    spin_unlock_irqrestore(&d->lock, flags);
    return 0;
}

/** End `d`'s polled claim and start its queue again, under the port's lock,
 * and wake the callers that sleep for the port; a command that could not be
 * built ends here too, or the queue would wait for a command that never
 * comes to advance it. */
static void ahci_release(struct ahci_disk *d)
{
    uint32_t flags = spin_lock_irqsave(&d->lock);

    d->polling = 0;
    ahci_start_next(d);
    spin_unlock_irqrestore(&d->lock, flags);
    if (wq_active(&d->claim_wq)) {
        wq_wake_all(&d->claim_wq);
    }
}

/**
 * End a polled command that failed, `why` saying how: recover the port and
 * log the failure, since the caller sees only `-EIO`. The claim's release
 * starts the queue behind it again, or every request in it would wait out
 * its deadline and the disk be declared stalled with nothing wrong. Returns
 * `-EIO`.
 */
static int ahci_polled_failed(struct ahci_disk *d, uint8_t command, const char *why)
{
    int rc = ahci_io_error(d);

    klog(KLOG_WARN, "ahci", "port %d: command 0x%X failed: %s", d->port, (unsigned)command, why);
    ahci_acknowledge(d);
    return rc;
}

/**
 * Issue one polled command on `d`'s port, which the caller has claimed
 * (`ahci_claim`), and spin to completion within `steps`, what is left of
 * the caller's bound once the claim was won: the wait for the task file to
 * go idle and the wait for the slot take from it in turn.
 *
 * Builds the command through ahci_build_command, waits for the task file to
 * go idle, sets the slot's `PxCI` bit, and busy-polls it clear, watching the
 * fatal `PxIS` bits for failure, then judges the task file's ERR and DF and
 * the byte count the HBA reports (`ahci_judge`), so a short transfer fails
 * instead of passing. The HBA performs the DMA; the processor only spins on
 * memory-mapped status, in bounded steps. It never sleeps, and allocates
 * only a page of the DMA pool for an entry that needs one (`ahci_piece`), so
 * it may be called with interrupts off, which the page-cache window's fault
 * needs.
 *
 * Returns 0 on success, `-EIO` on an error or an expiry, and -1 when the
 * command could not be built: a command the disk policy does not allow, or a
 * buffer the table cannot describe.
 */
static int ahci_command_claimed(struct ahci_disk *d, uint8_t command, uint64_t lba, uint32_t count,
                                volatile uint8_t *buf, uint32_t bytes, int write, uint32_t steps)
{
    uint32_t ci_off = ahci_port_off(d->port, AHCI_PxCI);
    uint32_t tfd_off = ahci_port_off(d->port, AHCI_PxTFD);
    uint32_t is_off = ahci_port_off(d->port, AHCI_PxIS);
    uint32_t spin;
    int rc;

    if (ahci_build_command(d, command, lba, count, buf, bytes, write) != 0) {
        return -1;
    }

    /* The task file must be idle before a command issues (AHCI 5.5.1). */
    spin = steps;
    while (ahci_read32(tfd_off) & (AHCI_PxTFD_BSY | AHCI_PxTFD_DRQ)) {
        block_count_poll();
        if (--spin == 0) {
            return ahci_polled_failed(d, command, "the device stayed busy");
        }
        ahci_poll_step();
    }

    ahci_write32(is_off, 0xFFFFFFFFu); /* clear stale status (write 1) */
    block_count_command();
    if (command == ATA_FLUSH_EXT) {
        block_count_flush();
    }
    ahci_write32(ci_off, 1u << AHCI_SLOT);

    while (ahci_read32(ci_off) & (1u << AHCI_SLOT)) {
        uint32_t is;

        block_count_poll();
        is = ahci_read32(is_off);
        block_count_poll();
        if (is & AHCI_PxIS_FATAL) {
            return ahci_polled_failed(d, command, "the controller reported an error");
        }
        if (--spin == 0) {
            return ahci_polled_failed(d, command,
                                      kirq_enabled()
                                          ? "no answer in thirty seconds"
                                          : "no answer in five seconds with interrupts off");
        }
        ahci_poll_step();
    }
    rc = ahci_judge_polled(d, bytes, write);
    ahci_acknowledge(d);
    return rc;
}

/**
 * Issue one polled command on `d`'s port and spin to completion, from any
 * context: claim the port, run the command (`ahci_command_claimed`), and
 * release it. On a port with a queued command active, the claim finishes
 * that command first by polling and completes its request, and the release
 * starts the queue again after this command, or after one that could not
 * be built. The bound is the caller's, thirty seconds with interrupts on
 * and five with them off, and covers the wait for another polled caller's
 * claim as well as the command. Returns as `ahci_command_claimed` does, and
 * `-EIO` when the other claim outlasts the bound.
 */
static int ahci_run_command(struct ahci_disk *d, uint8_t command, uint64_t lba, uint32_t count,
                            volatile uint8_t *buf, uint32_t bytes, int write)
{
    uint32_t steps = ahci_cmd_steps();
    int rc;

    if (ahci_claim(d, command, &steps) != 0) {
        return -EIO;
    }
    rc = ahci_command_claimed(d, command, lba, count, buf, bytes, write, steps);
    ahci_release(d);
    return rc;
}

/**
 * Describe `bytes` in the page frames `frames` in the table of the command
 * `ahci_build_command` built in `d`'s slot without a buffer: an entry per
 * frame with its address, as Linux's `ahci_fill_sg` writes an entry per
 * segment of the request's pages (drivers/ata/libahci.c).
 * `ahci_start` offers such reads only where every frame is in the
 * controller's reach (`BLOCK_FRAMES`). Returns 0, or -1 for more pages
 * than the table holds.
 */
static int ahci_frames_table(struct ahci_disk *d, const uintptr_t *frames, uint32_t bytes)
{
    volatile ahci_cmd_header_t *hdr = (volatile ahci_cmd_header_t *)d->cmdlist;
    volatile ahci_prdt_t *prdt = (volatile ahci_prdt_t *)(d->cmdtable + AHCI_PRDT_OFFSET);
    uint32_t n;

    if (bytes == 0 || bytes > AHCI_PRDT_MAX * PAGE_SIZE) {
        klog(KLOG_WARN, "ahci", "refused: a buffer the command table cannot describe");
        return -1;
    }
    for (n = 0; n * PAGE_SIZE < bytes; n++) {
        uint32_t chunk = bytes - n * PAGE_SIZE < PAGE_SIZE ? bytes - n * PAGE_SIZE : PAGE_SIZE;

        prdt[n].dba = (uint32_t)frames[n];
        prdt[n].dbau = ahci_hi((uint64_t)frames[n]);
        prdt[n].reserved = 0;
        prdt[n].dbc = (chunk - 1) & AHCI_DBC_MASK;
    }
    hdr->dw0 |= n << 16;
    return 0;
}

/*
 * The interrupt-driven path.
 *
 * A request's buffer is described as the polled path describes any buffer:
 * one PRDT entry for a buffer in i686's identity window, an entry per page
 * otherwise. One command is active per port; the rest wait in the port's
 * queue. The port's interrupt completes the active request and starts the
 * next, so the processor runs another process during a transfer and two
 * disks progress independently. A write is the transfer and then a flush,
 * unless its request has `BLOCK_REQ_NOFLUSH`, so a completed write is on the
 * medium as the polled path's is. Everything here runs under the port's
 * lock: `submit` takes it as libata's `ata_scsi_queuecmd` takes `ap->lock`
 * (drivers/ata/libata-scsi.c:4627-4648), and the interrupt as
 * `ahci_single_level_irq_intr` takes the host's (drivers/ata/
 * libahci.c:2023-2056), with no big lock.
 */

/** Issue the command built in `d`'s slot without waiting, by its bit in
 * PxCI alone, as Linux's `ahci_qc_issue` (drivers/ata/libahci.c:2061-2084):
 * the interrupt or the polled path that ended the last command has cleared
 * the port's status already. The port's lock is held. */
static void ahci_issue(struct ahci_disk *d)
{
    block_count_command();
    ahci_write32(ahci_port_off(d->port, AHCI_PxCI), 1u << AHCI_SLOT);
}

/**
 * Start the request `r` on `d`: a read, the transfer half of a write, or a
 * flush alone. Returns 0, or -1 when the command could not be built. The
 * port's lock is held and the port is idle.
 */
static int ahci_start_request(struct ahci_disk *d, struct block_request *r)
{
    uint32_t bytes = r->count * BLOCK_SECTOR_SIZE;

    if (r->flags & BLOCK_REQ_FLUSH) {
        if (ahci_build_command(d, ATA_FLUSH_EXT, 0, 0, 0, 0, 0) != 0) {
            return -1;
        }
        d->flushing = 1;
        block_count_flush();
    } else {
        if (ahci_build_command(d, r->write ? ATA_WRITE_DMA_EXT : ATA_READ_DMA_EXT, r->lba, r->count,
                               (volatile uint8_t *)r->page, r->frames ? 0 : bytes, r->write) != 0 ||
            (r->frames && ahci_frames_table(d, r->frames, bytes) != 0)) {
            return -1;
        }
        d->flushing = 0;
    }
    d->active = r;
    d->active_generation = r->generation;
    ahci_issue(d);
    return 0;
}

/**
 * The active command has left the slot: judge it, acknowledge its interrupt,
 * then finish its request or start the flush half of a write. A write whose
 * request has `BLOCK_REQ_NOFLUSH` is finished at its transfer, and the disk is
 * marked `unflushed` until a flush, of a write or alone, has finished.
 * Runs from the interrupt, which has read and cleared the port's status
 * into `is` and passes `irq` nonzero, so that the command is judged by that
 * status and the received D2H FIS; and from the polled path draining the
 * port, with `irq` zero, which reads the registers and acknowledges here.
 * Under the port's lock, inside which the request is completed.
 */
static void ahci_finish_active(struct ahci_disk *d, int irq, uint32_t is)
{
    struct block_request *r = d->active;
    uint32_t bytes;
    int rc;

    if (!r) {
        return;
    }
    bytes = d->flushing ? 0 : r->count * BLOCK_SECTOR_SIZE;
    if (irq) {
        rc = ahci_judge(d, is, d->recv_fis[AHCI_RFIS_STATUS], bytes, r->write);
    } else {
        rc = ahci_judge_polled(d, bytes, r->write);
        ahci_acknowledge(d);
    }
    if (rc == 0 && r->write && !d->flushing && (r->flags & BLOCK_REQ_NOFLUSH)) {
        d->dev.unflushed = 1;
    } else if (rc == 0 && r->write && !d->flushing) {
        if (ahci_build_command(d, ATA_FLUSH_EXT, 0, 0, 0, 0, 0) == 0) {
            d->flushing = 1;
            block_count_flush();
            ahci_issue(d);
            return;
        }
        rc = -EIO;
    } else if (rc == 0 && d->flushing) {
        d->dev.unflushed = 0;
    }
    d->active = 0;
    latency_span(LAT_DISK, r->taken_us, timer_now_us());
    block_request_complete(r, d->active_generation, rc);
}

/**
 * Start queued requests, oldest first, while the port is idle and no polled
 * command has claimed it: until one is active, or none is left. A request
 * whose command cannot be built is completed with `-EIO`.
 *
 * The port's lock is held across the whole of it. Taking a request off the
 * queue makes it the active one before its command is issued, and an
 * interrupt arriving in that window, on any processor, would find a request
 * whose command has not been sent and the slot's bit clear, and complete it
 * as though the disk had answered; the queue would then hold requests
 * nothing starts, each waiting out its deadline.
 */
static void ahci_start_next(struct ahci_disk *d)
{
    while (!d->active && !d->polling && d->queue_head) {
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
 * Finish the active command by polling, under the port's lock with
 * interrupts masked, for a polled caller that needs the port at once and
 * may be unable to take an interrupt: the fault path with interrupts off.
 * A write's flush half is finished too. A command that does not leave the
 * slot within five seconds has its request completed with `-EIO` after the
 * port's recovery. The interrupt that would have completed the command
 * finds nothing active. Each command found active is counted for
 * `disk-drains` (`block_count_drain`).
 */
static void ahci_drain(struct ahci_disk *d)
{
    uint32_t ci_off = ahci_port_off(d->port, AHCI_PxCI);
    uint32_t is_off = ahci_port_off(d->port, AHCI_PxIS);

    if (d->active) {
        block_count_drain();
    }
    while (d->active) {
        uint32_t spin = AHCI_CMD_STEPS_MASKED;

        for (;;) {
            uint32_t ci = ahci_read32(ci_off);
            uint32_t is;

            block_count_poll();
            if (!(ci & (1u << AHCI_SLOT))) {
                break;
            }
            is = ahci_read32(is_off);
            block_count_poll();
            if (is & AHCI_PxIS_FATAL) {
                break;
            }
            if (--spin == 0) {
                break;
            }
            ahci_poll_step();
        }
        if (spin == 0) {
            struct block_request *r = d->active;

            (void)ahci_io_error(d);
            d->active = 0;
            block_request_complete(r, d->active_generation, -EIO);
            break;
        }
        ahci_finish_active(d, 0, 0);
    }
}

/** The device's `submit`: queue `r`, and start it when the port is idle,
 * under the port's lock, which is held only for that. Returns 0, or `-EIO`
 * for a range past the disk. */
static int ahci_submit(block_device_t *device, struct block_request *r)
{
    struct ahci_disk *d = (struct ahci_disk *)device->drvdata;
    uint32_t flags;

    if (r->count == 0 || r->lba > device->sector_count ||
        r->count > device->sector_count - r->lba) {
        return -EIO;
    }
    r->queued = 0;
    flags = spin_lock_irqsave(&d->lock);
    if (d->queue_tail) {
        d->queue_tail->queued = r;
    } else {
        d->queue_head = r;
    }
    d->queue_tail = r;
    ahci_start_next(d);
    spin_unlock_irqrestore(&d->lock, flags);
    return 0;
}

/** The device's `flush`: queue `r`, a flush alone, as `submit` queues a
 * transfer, or with `r` NULL flush at once by polling, for a caller that
 * cannot sleep. */
static int ahci_flush_submit(block_device_t *device, struct block_request *r)
{
    if (!r) {
        return ahci_flush((struct ahci_disk *)device->drvdata);
    }
    r->flags |= BLOCK_REQ_FLUSH;
    return ahci_submit(device, r);
}

/**
 * One port's part of the interrupt, under its lock: the port's interrupt
 * status read and cleared first, as Linux's `ahci_port_intr`
 * (drivers/ata/libahci.c:1970-1979), then a command that has left the slot
 * finished and the next started. A fatal error with the command still in
 * the slot recovers the port and fails the request. A port a polled command
 * has claimed is left to its caller. Returns 1 when the port was handled,
 * so that its bit of the controller's status is cleared, and 0 for one left
 * to its polled caller.
 */
static int ahci_port_interrupt(struct ahci_disk *d)
{
    uint32_t is_off = ahci_port_off(d->port, AHCI_PxIS);
    uint32_t is;

    if (d->polling) {
        return 0;
    }
    is = ahci_read32(is_off);
    if (is == 0) {
        return 1;
    }
    ahci_write32(is_off, is);
    if (d->active && !(ahci_read32(ahci_port_off(d->port, AHCI_PxCI)) & (1u << AHCI_SLOT))) {
        ahci_finish_active(d, 1, is);
        ahci_start_next(d);
        return 1;
    }
    if (d->active && (is & AHCI_PxIS_FATAL)) {
        struct block_request *r = d->active;

        (void)ahci_io_error(d);
        d->active = 0;
        block_request_complete(r, d->active_generation, -EIO);
        ahci_start_next(d);
    }
    return 1;
}

/**
 * The controller's interrupt, which runs without the big lock
 * (ISR_UNLOCKED): each port with a disk whose bit the controller's status
 * has is handled under its own lock (`ahci_port_interrupt`), and the bits
 * of the ports handled are cleared after them, as Linux's
 * `ahci_single_level_irq_intr` reads HOST_IRQ_STAT once, handles the ports
 * it names and writes it back (drivers/ata/libahci.c:1997-2019, 2023-2056).
 * A port a polled command has claimed is left set for its caller, whose
 * acknowledgement clears it.
 */
static void ahci_handler(isr_params_t *params)
{
    uint32_t pending = ahci_read32(AHCI_IS), done = 0;
    int i;

    (void)params;
    for (i = 0; i < ahci_ndisks && pending; i++) {
        struct ahci_disk *d = &ahci_disks[i];
        uint32_t flags;

        if (!(pending & (1u << d->port))) {
            continue;
        }
        flags = spin_lock_irqsave(&d->lock);
        if (ahci_port_interrupt(d)) {
            done |= 1u << d->port;
        }
        spin_unlock_irqrestore(&d->lock, flags);
    }
    if (done) {
        ahci_write32(AHCI_IS, done);
    }
}

void ahci_start(void)
{
    int i;

    if (!ahci_base || ahci_ndisks == 0 || kcmdline_has("rach2.ahcipoll")) {
        return;
    }
    if (!isr_msi_ready() || (ahci_vector = isr_alloc_vector()) < 0) {
        klog(KLOG_INFO, "ahci", "polled: no vector for the controller's messages");
        return;
    }
    register_isr_handler((uint8_t)ahci_vector, ahci_handler, "ahci");
    isr_register_flags((uint8_t)ahci_vector, ahci_handler, ISR_UNLOCKED);
    if (pci_msi_enable(&ahci_pci, (uint8_t)ahci_vector) != 0) {
        klog(KLOG_INFO, "ahci", "polled: the controller has no message capability");
        isr_free_vector(ahci_vector);
        ahci_vector = -1;
        return;
    }
    for (i = 0; i < ahci_ndisks; i++) {
        struct ahci_disk *d = &ahci_disks[i];

        ahci_write32(ahci_port_off(d->port, AHCI_PxIS), 0xFFFFFFFFu);
        ahci_write32(ahci_port_off(d->port, AHCI_PxIE), AHCI_PxIE_WANTED);
        d->dev.flags &= ~BLOCK_FAULT_SAFE;
        /* A frame above 4 GiB needs the controller's 64-bit addressing. */
        if (ahci_dma64 || sizeof(uintptr_t) == 4) {
            d->dev.flags |= BLOCK_FRAMES;
        }
        d->dev.submit = ahci_submit;
        d->dev.flush = ahci_flush_submit;
    }
    ahci_write32(AHCI_IS, 0xFFFFFFFFu);
    ahci_write32(AHCI_GHC, ahci_read32(AHCI_GHC) | AHCI_GHC_IE);
    klog(KLOG_INFO, "ahci", "interrupts: messages on vector %d, %d disks queued", ahci_vector,
         ahci_ndisks);
}

/* The last IDENTIFY answer, which the registry reads the strings from. */
static volatile uint16_t ahci_idbuf[BLOCK_SECTOR_SIZE / 2];

/**
 * IDENTIFY `d` by polling, into `ahci_idbuf`, and record its 48-bit sector
 * count (words 100-103). Returns 0, or -1 with `ahci_err` set for a failed
 * IDENTIFY, a device whose logical sector is not 512 bytes (word 106 bit 14
 * valid and bit 12 set), which the block math needs, or a capacity of none,
 * past 48 bits, or of 48 one bits, which is a device that did not answer.
 */
static int ahci_identify(struct ahci_disk *d)
{
    volatile uint16_t *w = ahci_idbuf;

    if (ahci_run_command(d, ATA_IDENTIFY, 0, 0, (volatile uint8_t *)ahci_idbuf, BLOCK_SECTOR_SIZE,
                         0) != 0) {
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
    /* 48 one bits is a device that did not answer, not a disk of 144 PB. */
    if ((d->sectors >> 48) != 0 || d->sectors == 0xFFFFFFFFFFFFull) {
        ahci_err = "the device reported an impossible capacity";
        return -1;
    }
    return 0;
}

/**
 * Flush the device's write cache by polling, so a completed write is on the
 * medium, as the buffer cache's dirty flags and `pc_sync` expect. Returns as
 * `ahci_run_command` does. The `unflushed` mark is cleared under the claim,
 * which keeps the queue and the interrupt off the port, and so every write
 * answered before it is under this flush; a failed flush sets it again,
 * and a write answered once the claim is released sets it after.
 */
static int ahci_flush(struct ahci_disk *d)
{
    uint32_t steps = ahci_cmd_steps();
    int rc;

    if (ahci_claim(d, ATA_FLUSH_EXT, &steps) != 0) {
        return -EIO; /* `unflushed` stays as it was */
    }
    d->dev.unflushed = 0;
    rc = ahci_command_claimed(d, ATA_FLUSH_EXT, 0, 0, 0, 0, 0, steps);
    if (rc != 0) {
        d->dev.unflushed = 1;
    }
    ahci_release(d);
    return rc;
}

/** The device's `read`: a polled `READ DMA EXT` of `count` sectors from
 * `lba`, from any context. */
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
 * The device's `write`: a polled `WRITE DMA EXT`, then `FLUSH CACHE EXT` for
 * durability, from any context.
 */
static int ahci_write(block_device_t *device, uint64_t lba, uint32_t count, const void *buffer)
{
    struct ahci_disk *d = (struct ahci_disk *)device->drvdata;
    int rc;

    if (count == 0 || lba > device->sector_count || count > device->sector_count - lba ||
        count > AHCI_MAX_SECTORS_PER_CMD) {
        return -EIO;
    }
    rc = ahci_run_command(d, ATA_WRITE_DMA_EXT, lba, count, (volatile uint8_t *)(uintptr_t)buffer,
                          count * BLOCK_SECTOR_SIZE, 1);
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
 *
 * `block0` is cleared before anything else, so a failed probe leaves no
 * published device on a controller it has reset, whoever called it;
 * `ata_init` differs, and keeps a `block0` another driver has published. A
 * bring-up that fails dumps the registers to the console (`ahci_dump`), so
 * that one failed boot shows the state behind its failure.
 */
int ahci_init(void)
{
    int i, published = 0;

    block0 = 0;
    ahci_base = 0;
    ahci_port = -1;
    ahci_ndisks = 0;
    ahci_vector = -1;
    if (ahci_discover() != 0) {
        klog(KLOG_INFO, "ahci", "none: %s", ahci_err);
        return -1;
    }
    ahci_dma64 = (ahci_read32(AHCI_CAP) & AHCI_CAP_S64A) != 0 && !kcmdline_has("rach2.dma32");
    if (!ahci_dma64) {
        klog(KLOG_INFO, "ahci",
             "32-bit addresses: a buffer above 4 GiB moves through the DMA pool");
    }
    if (ahci_hba_own() != 0 || ahci_quiesce_ports() != 0 || ahci_select_port() != 0) {
        klog(KLOG_ERR, "ahci", "unusable: %s", ahci_err);
        if (ahci_base) {
            ahci_dump();
        }
        return -1;
    }

    for (i = 0; i < ahci_ndisks; i++) {
        struct ahci_disk *d = &ahci_disks[i];

        spin_lock_init(&d->lock, "ahci_port");
        d->polling = 0;
        d->active = 0;
        d->queue_head = d->queue_tail = 0;
        if (!ahci_dma64 && !d->bounce) {
            d->bounce = kzalloc(sizeof(*d->bounce));
        }
        if (!ahci_dma64 && !d->bounce) {
            klog(KLOG_ERR, "ahci", "port %d: unusable: no memory for its bounce record", d->port);
            continue;
        }
        if (ahci_port_start(d) != 0 || ahci_identify(d) != 0) {
            klog(KLOG_ERR, "ahci", "port %d: unusable: %s", d->port, ahci_err);
            (void)ahci_port_stop(d->port);
            continue;
        }
        d->dev.sector_size = BLOCK_SECTOR_SIZE;
        d->dev.sector_count = d->sectors;
        d->dev.max_sectors = AHCI_REQ_SECTORS;
        d->dev.flags = BLOCK_FAULT_SAFE | BLOCK_POLLABLE | BLOCK_WRITE_THROUGH;
        d->dev.unflushed = 0;
        d->dev.read = ahci_read;
        d->dev.write = ahci_write;
        d->dev.submit = 0;
        d->dev.flush = 0;
        d->dev.backing = 0;
        d->dev.drvdata = d;
        if (!block0) {
            block0 = &d->dev;
            ahci_port = d->port;
        }
        disk_register(&d->dev, "ahci", d->port, ahci_idbuf);
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

/**
 * CAP2 says whether a handoff exists, BOHC how it went, and PCS whether the
 * Intel port PHYs are enabled at all: each is a cause of a machine that finds
 * no disk, so the dump has all three.
 */
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
    console_printf("ahci: CAP2=%X BOHC=%X PCS=%X\n", ahci_read32(AHCI_CAP2), ahci_read32(AHCI_BOHC),
                   (unsigned)pci_read_config16(ahci_pci.bus, ahci_pci.device, ahci_pci.function,
                                               AHCI_INTEL_PCS));
    for (port = 0; port < ahci_ndisks; ++port) {
        console_printf("ahci: port %d dma cmdlist=%X fis=%X table=%X\n", ahci_disks[port].port,
                       (uint32_t)virt_to_phys(ahci_disks[port].cmdlist),
                       (uint32_t)virt_to_phys(ahci_disks[port].recv_fis),
                       (uint32_t)virt_to_phys(ahci_disks[port].cmdtable));
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
