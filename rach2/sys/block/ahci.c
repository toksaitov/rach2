#include "ahci.h"
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

/* Task-file status bits (PxTFD low byte) and the port's error interrupt bit. */
#define AHCI_PxTFD_BSY 0x80u
#define AHCI_PxTFD_DRQ 0x08u
#define AHCI_PxTFD_ERR 0x01u
#define AHCI_PxIS_TFES 0x40000000u /* task file error status */

/* The H2D Register FIS and the ATA commands the polled path issues. */
#define AHCI_FIS_H2D 0x27u
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
#define AHCI_DBC_MASK    0x3FFFFFu  /* PRDT byte count, bits 0-21 */

/* Typed overlays on the DMA byte regions: one command-list header (the HBA
   reads 32 bytes per slot, of which only the first 16 are software-set; the
   rest stay zero), the H2D FIS at the command-table head, and one PRDT entry. */
typedef struct {
    uint32_t dw0;   /* CFL[4:0], W[6], PRDTL[31:16] */
    uint32_t prdbc; /* byte count the HBA writes back */
    uint32_t ctba;  /* command table base, low 32 */
    uint32_t ctbau; /* command table base, high 32 */
} __attribute__((packed)) ahci_cmd_header_t;

typedef struct {
    uint8_t fis_type;         /* 0x27 */
    uint8_t pmport_c;         /* C bit and port-multiplier port */
    uint8_t command;
    uint8_t featurel;
    uint8_t lba0, lba1, lba2; /* LBA 7:0, 15:8, 23:16 */
    uint8_t device;           /* 0x40 for LBA mode */
    uint8_t lba3, lba4, lba5; /* LBA 31:24, 39:32, 47:40 */
    uint8_t featureh;
    uint8_t countl, counth;
    uint8_t icc;
    uint8_t control;
    uint32_t aux;             /* reserved */
} __attribute__((packed)) ahci_h2d_fis_t;

typedef struct {
    uint32_t dba;      /* data base, low 32 */
    uint32_t dbau;     /* data base, high 32 */
    uint32_t reserved;
    uint32_t dbc;      /* byte count minus one [21:0], interrupt bit [31] */
} __attribute__((packed)) ahci_prdt_t;

/* The logical sector count from IDENTIFY words 100-103, 0 until known. */
static uint64_t ahci_sector_count;

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

/* Cache/DMA coherency, real-hardware honesty mirroring the HDA driver's helper.
   x86 bus-master DMA snoops the caches, so this is a no-op in practice and a
   plain no-op on emulators (which report no CLFLUSH). */
static bool ahci_have_clflush = false;
static uint32_t ahci_clflush_size = 64;

/* Record whether CLFLUSH is available and its line size (CPUID leaf 1). */
static void ahci_detect_cache(void)
{
    uint32_t eax, ebx, ecx, edx;
    uint32_t sz;

    eax = 1;
    __asm__ __volatile__("cpuid"
                         : "+a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
                         :
                         : "memory");

    ahci_have_clflush = ((edx & (1u << 19)) != 0);
    sz = ((ebx >> 8) & 0xFFu) * 8u;
    if (sz == 0) {
        sz = 64;
    }
    ahci_clflush_size = sz;
}

/* Evict [addr, addr+size) so a DMA write the CPU is about to read, or a
   descriptor the HBA is about to read, crosses the cache a line at a time. */
static void ahci_dma_sync(const volatile void *addr, uint32_t size)
{
    uintptr_t p, end, line;

    if (!addr || size == 0) {
        return;
    }
    if (ahci_have_clflush && ahci_clflush_size != 0) {
        line = (uintptr_t) ahci_clflush_size;
        p = (uintptr_t) addr & ~(line - 1u);
        end = (uintptr_t) addr + (uintptr_t) size;
        while (p < end) {
            __asm__ __volatile__("clflush (%0)" :: "r"(p) : "memory");
            p += line;
        }
        __asm__ __volatile__("lock; addl $0, 0(%%esp)" ::: "memory");
    } else {
        __asm__ __volatile__("wbinvd" ::: "memory");
    }
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
    /* Map the ABAR cache-disabled (PCD|PWT) whatever its address. A low ABAR is
       already identity-mapped but cacheable, so a status poll could read stale;
       map_mmio rewrites the PTE cache-disabled either way. */
    if (map_mmio(abar, AHCI_ABAR_MAP_SIZE) == 0) {
        return -1;
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

/* Zero `n` bytes of a DMA region. */
static void ahci_zero(volatile uint8_t *p, uint32_t n)
{
    uint32_t i;

    for (i = 0; i < n; ++i) {
        p[i] = 0;
    }
}

/* Issue one polled command on the selected port and spin to completion.
 *
 * Builds the command-list header, a Register H2D FIS, and a single PRDT entry
 * over the static DMA regions, waits for the task file to go idle, sets the
 * slot's `PxCI` bit, and busy-polls it clear while watching `PxIS.TFES` and
 * `PxTFD.STS.ERR` for a device error. The HBA performs the DMA; the CPU only
 * spins on memory-mapped status, bounded by the iteration watchdog. So it
 * sleeps and allocates nothing and is callable with interrupts off, the
 * contract the page-cache window fault needs. `buf` must be physically
 * contiguous and below 4 GiB, which every identity-mapped buffer below
 * HARD_MEMORY_LIMIT is. Returns 0 on success, `-EIO` on error or expiry. */
static int ahci_run_command(uint8_t command, uint64_t lba, uint32_t count,
                            volatile uint8_t *buf, uint32_t bytes, int write)
{
    volatile ahci_cmd_header_t *hdr =
        (volatile ahci_cmd_header_t *) ahci_cmdlist;
    volatile ahci_h2d_fis_t *fis = (volatile ahci_h2d_fis_t *) ahci_cmdtable;
    volatile ahci_prdt_t *prdt =
        (volatile ahci_prdt_t *) (ahci_cmdtable + AHCI_PRDT_OFFSET);
    uint32_t ci_off = ahci_port_off(ahci_port, AHCI_PxCI);
    uint32_t tfd_off = ahci_port_off(ahci_port, AHCI_PxTFD);
    uint32_t is_off = ahci_port_off(ahci_port, AHCI_PxIS);
    uint32_t prdtl = (bytes > 0) ? AHCI_PRDTL_ONE : 0u;
    uint32_t spin;

    ahci_zero(ahci_cmdtable, AHCI_CMDTABLE_SIZE);

    fis->fis_type = AHCI_FIS_H2D;
    fis->pmport_c = AHCI_FIS_H2D_C;
    fis->command = command;
    fis->featurel = 0;
    fis->lba0 = (uint8_t) lba;
    fis->lba1 = (uint8_t) (lba >> 8);
    fis->lba2 = (uint8_t) (lba >> 16);
    fis->device = ATA_DEV_LBA;
    fis->lba3 = (uint8_t) (lba >> 24);
    fis->lba4 = (uint8_t) (lba >> 32);
    fis->lba5 = (uint8_t) (lba >> 40);
    fis->featureh = 0;
    fis->countl = (uint8_t) count;
    fis->counth = (uint8_t) (count >> 8);
    fis->icc = 0;
    fis->control = 0;
    fis->aux = 0;

    if (bytes > 0) {
        prdt->dba = (uint32_t) (uintptr_t) buf;
        prdt->dbau = 0;
        prdt->reserved = 0;
        prdt->dbc = (bytes - 1) & AHCI_DBC_MASK;
    }

    hdr->dw0 = AHCI_CFL_DWORDS | (write ? AHCI_CMD_W : 0u) | prdtl;
    hdr->prdbc = 0;
    hdr->ctba = (uint32_t) (uintptr_t) ahci_cmdtable;
    hdr->ctbau = 0;

    ahci_dma_sync(ahci_cmdtable, AHCI_CMDTABLE_SIZE);
    ahci_dma_sync(ahci_cmdlist, sizeof(ahci_cmd_header_t));
    if (write && bytes > 0) {
        ahci_dma_sync(buf, bytes);
    }

    /* The task file must be idle before a command issues (AHCI 5.5.1). */
    spin = AHCI_SPIN_LIMIT;
    while (ahci_read32(tfd_off) & (AHCI_PxTFD_BSY | AHCI_PxTFD_DRQ)) {
        if (--spin == 0) {
            return -EIO;
        }
    }

    ahci_write32(is_off, 0xFFFFFFFFu); /* clear stale status (write 1) */
    ahci_write32(ci_off, 1u << AHCI_SLOT);

    spin = AHCI_SPIN_LIMIT;
    while (ahci_read32(ci_off) & (1u << AHCI_SLOT)) {
        if (ahci_read32(is_off) & AHCI_PxIS_TFES) {
            return -EIO;
        }
        if (--spin == 0) {
            return -EIO;
        }
    }
    if (ahci_read32(tfd_off) & AHCI_PxTFD_ERR) {
        return -EIO;
    }

    if (!write && bytes > 0) {
        ahci_dma_sync(buf, bytes); /* evict stale lines; CPU reads fresh */
    }
    return 0;
}

/* IDENTIFY DEVICE and record the 48-bit logical sector count (words 100-103).
   Clamped to 512-byte sectors, which KRFFS sector math assumes. Returns 0 on
   success, -1 on a failed IDENTIFY or an empty device. */
static int ahci_identify(void)
{
    static volatile uint8_t idbuf[BLOCK_SECTOR_SIZE] __attribute__((aligned(2)));
    volatile uint16_t *w = (volatile uint16_t *) idbuf;

    if (ahci_run_command(ATA_IDENTIFY, 0, 0, idbuf, BLOCK_SECTOR_SIZE, 0) != 0) {
        return -1;
    }
    ahci_sector_count = (uint64_t) w[100] |
                        ((uint64_t) w[101] << 16) |
                        ((uint64_t) w[102] << 32) |
                        ((uint64_t) w[103] << 48);
    if (ahci_sector_count == 0) {
        return -1;
    }
    return 0;
}

/* Flush the device write cache so a completed write reaches durable media,
   the durability the dirty-flag and pc_sync write-back contract relies on. */
static int ahci_flush(void)
{
    return ahci_run_command(ATA_FLUSH_EXT, 0, 0, 0, 0, 0);
}

/* block0->read: a polled READ DMA EXT of `count` sectors from `lba`. */
static int ahci_read(block_device_t *device, uint64_t lba, uint32_t count,
                     void *buffer)
{
    if (count == 0 || lba > device->sector_count ||
        count > device->sector_count - lba ||
        count > AHCI_MAX_SECTORS_PER_CMD) {
        return -EIO;
    }
    return ahci_run_command(ATA_READ_DMA_EXT, lba, count,
                            (volatile uint8_t *) buffer,
                            count * BLOCK_SECTOR_SIZE, 0);
}

/* block0->write: a polled WRITE DMA EXT, then FLUSH CACHE EXT for durability. */
static int ahci_write(block_device_t *device, uint64_t lba, uint32_t count,
                      const void *buffer)
{
    int rc;

    if (count == 0 || lba > device->sector_count ||
        count > device->sector_count - lba ||
        count > AHCI_MAX_SECTORS_PER_CMD) {
        return -EIO;
    }
    rc = ahci_run_command(ATA_WRITE_DMA_EXT, lba, count,
                          (volatile uint8_t *) (uintptr_t) buffer,
                          count * BLOCK_SECTOR_SIZE, 1);
    if (rc != 0) {
        return rc;
    }
    return ahci_flush();
}

static block_device_t ahci_device;

int ahci_init(void)
{
    ahci_base = 0;
    ahci_port = -1;
    ahci_sector_count = 0;
    ahci_detect_cache();
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
    if (ahci_identify() != 0) {
        return -1;
    }

    ahci_device.sector_size = BLOCK_SECTOR_SIZE;
    ahci_device.sector_count = ahci_sector_count;
    ahci_device.read = ahci_read;
    ahci_device.write = ahci_write;
    ahci_device.drvdata = 0;
    block0 = &ahci_device;

    serial_printf("ahci: port %d: %d sectors, %d MiB\n", ahci_port,
                  (uint32_t) ahci_sector_count,
                  (uint32_t) (ahci_sector_count / 2048));
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
    serial_printf("ahci: lba 0 signature %X %X\n",
                  (uint32_t) buf[510], (uint32_t) buf[511]);
    if (buf[510] == 0x55 && buf[511] == 0xAA) {
        serial_printf("ahci: selftest: MBR signature OK\n");
    } else {
        serial_printf("ahci: selftest: MBR signature mismatch\n");
    }
}
