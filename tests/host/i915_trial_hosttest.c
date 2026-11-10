/**
 * i915_trial_hosttest.c: the path of the Intel graphics driver's first
 * trial on the Lenovo, 78-4b, run whole against the host model of its HD
 * 4400 as the firmware leaves it: the bind under rach2.i915 alone, the
 * trial's first boot, and under rach2.i915 and rach2.i915.firstlight, its
 * second.
 *
 * host-deps: rach2/sys/drm/i915/i915_hw.c tests/host/i915_model.c
 * host-deps: rach2/sys/drm/i915/i915_uncore.c rach2/sys/drm/i915/i915_power.c
 * host-deps: rach2/sys/drm/i915/i915_stolen.c rach2/sys/drm/i915/i915_gtt.c
 * host-deps: rach2/sys/drm/i915/i915_irq.c rach2/sys/drm/i915/i915_ring.c
 * host-deps: rach2/sys/drm/i915/i915_ppgtt.c
 * host-deps: rach2/sys/drm/i915/i915_firstlight.c
 * host-deps: rach2/sys/lib/kstring.c rach2/sys/lib/kctypes.c
 * host-deps: rach2/sys/mm/frame.c
 * host-deps: tests/host/compat/spinlock_host.c
 * host-deps: rach2/sys/drm/i915/i915_rc6.c rach2/sys/drm/i915/i915_rps.c
 * host-deps: rach2/sys/tick.c
 * host-deps: rach2/sys/drm/i915/intel_bios.c
 * host-deps: rach2/sys/drm/i915/intel_opregion.c
 * host-deps: rach2/sys/drm/i915/i915_aperture.c rach2/sys/drm/i915/i915_fence.c
 * host-inputs: tests/fixtures/lenovo-gpu/i915_opregion.bin
 *
 * i915_pci.c is included whole, and the driver is bound through
 * `i915_driver_bind` as the probe binds it, over the model's registers
 * seen through a boundary of the test's, which notes every register and
 * GTT entry written before passing the access on. The model holds what
 * gfxtool read on the Lenovo (tests/fixtures/lenovo-gpu/firmware-dump.txt):
 * the display power well on at the BIOS's request, the panel on the eDP
 * transcoder from pipe A, pipe A's plane scanning the firmware's
 * framebuffer at GTT offset 0, the whole of stolen memory in the GTT, and
 * an unclaimed access latched and PM's interrupts enabled.
 * The heap's pages have physical addresses of their own, which the
 * model's rings reach through the test; any other address stops the
 * ring. The IOMMU names the Lenovo's RMRR and translates nothing, as with
 * intel_iommu=off. The firmware's memory holds the Lenovo's OpRegion at
 * ASLS, read from its fixture, and nothing else. The processor takes the
 * device's messages whenever the model's clock moves, or, in a second run
 * of each row, leaves them latched, as with its interrupts off.
 *
 * Each row checks what makes the trial safe for the machine and its
 * screen: no wait reached its bound; nothing outside the GT was written
 * but the interrupt registers, the GT FIFO's and GTT's flush, the power
 * well's takeover and the pcode mailbox, and in the GT nothing but
 * forcewake, the four rings and their status pages, and RC6's and the
 * frequency's registers, which Linux writes as the driver does; no write
 * reached the firmware's 1032 framebuffer entries, and the plane, pipe
 * and eDP transcoder read as the firmware left them; the power well was
 * never released; the chip reached no memory but the driver's own pages;
 * and after the bind every interrupt source is masked, only the master
 * enable and the rings' user interrupts are enabled, and the firmware's
 * unclaimed access is cleared. The bind also decodes the Lenovo's VBT
 * from the OpRegion, and the release unmaps it.
 *
 * Returns 0 when every row passes, 1 otherwise.
 */

#include <stdarg.h>

#include "drm/i915/i915_pci.c"
#include "fs/sysfs.h"
#include "iommu.h"
#include "lib/kirq.h"
#include "mm/paging.h"
#include "proc/percpu.h"
#include "rtc.h"
#include "udelay.h"
#include "i915_model.h"
#include "i915_lenovo.h"
#include "i915_model_base.h"
#include "i915_model_ring.h"

extern int printf(const char *format, ...);
extern int vsnprintf(char *s, unsigned long n, const char *format, va_list ap);
extern int posix_memalign(void **p, unsigned long align, unsigned long size);
extern void *calloc(unsigned long count, unsigned long size);
extern void *malloc(unsigned long size);
extern void free(void *p);
extern char *strstr(const char *s, const char *t);
extern int strcmp(const char *a, const char *b);
extern int open(const char *path, int flags, ...);
extern long read(int fd, void *buf, unsigned long n);
extern int close(int fd);

static int fails;

#define CHECK(what, cond)                                \
    do {                                                 \
        if (!(cond)) {                                   \
            printf("i915-trial-test: fail: %s\n", what); \
            fails++;                                     \
        }                                                \
    } while (0)

/* ---- the kernel around the driver ---- */

static struct i915_model *bus; /* the chip on the bus, or NULL */
static int opt_i915, opt_firstlight;
static int irq_on, irq_depth;
static struct drm_device *last_registered;
static int vectors;
static char log_text[16384];

/** The boot log, kept whole for the rows to read. */
void klog(int level, const char *subsystem, const char *format, ...)
{
    char line[512];
    unsigned long n = 0, k;
    va_list ap;

    (void)level;
    (void)subsystem;
    va_start(ap, format);
    vsnprintf(line, sizeof(line), format, ap);
    va_end(ap);
    while (log_text[n]) {
        n++;
    }
    for (k = 0; line[k] && n + 2 < sizeof(log_text); k++) {
        log_text[n++] = line[k];
    }
    log_text[n++] = '\n';
    log_text[n] = 0;
}

/** Whether the log holds `text`. */
static int logged(const char *text)
{
    return strstr(log_text, text) != 0;
}

int kcmdline_has(const char *flag)
{
    return (opt_i915 && strcmp(flag, "rach2.i915") == 0) ||
           (opt_firstlight && strcmp(flag, "rach2.i915.firstlight") == 0);
}

/** The fake chip, which i915_fake_hosttest.c tests: no command line here
 * names it, so the probe never runs. */
int32_t i915_fake_probe(void)
{
    return 0;
}

/** The card's directory in /sys: none, so the driver adds no files. */
int drm_sysfs_card_dir(const struct drm_device *dev)
{
    (void)dev;
    return -ENOENT;
}

int sysfs_add_dir(int parent, const char *name)
{
    (void)parent;
    (void)name;
    return -ENOENT;
}

int sysfs_add_attr(int parent, const struct sysfs_attr *attr, void *obj, int arg)
{
    (void)parent;
    (void)attr;
    (void)obj;
    (void)arg;
    return -ENOENT;
}

int sysfs_node_child(int dir, const char *name, int len)
{
    (void)dir;
    (void)name;
    (void)len;
    return -ENOENT;
}

void k_udelay(uint32_t usec)
{
    (void)usec;
}

/* The MMIO backend, which the bind through the model does not use. */
uintptr_t map_mmio_typed(uintptr_t phys_addr, size_t size, pte_t cache)
{
    (void)phys_addr;
    (void)size;
    (void)cache;
    return 0;
}

/* The firmware's memory: the Lenovo's OpRegion at ASLS, as the fixture
 * holds it, with the maps and unmaps of it counted. */
static uint8_t opregion[OPREGION_SIZE];
static int fw_maps, fw_unmaps;

/** The OpRegion's bytes for a map of them, 0 for any other firmware
 * memory, the option ROM's shadow among them. */
uintptr_t mmio_map(uint64_t phys, size_t size, pte_t cache)
{
    (void)cache;
    if (phys >= LENOVO_GPU_ASLS && phys + size <= (uint64_t)LENOVO_GPU_ASLS + sizeof(opregion)) {
        fw_maps++;
        return (uintptr_t)(opregion + (phys - LENOVO_GPU_ASLS));
    }
    return 0;
}

/** Count an unmap of the OpRegion. */
void mmio_unmap(uintptr_t va, size_t size)
{
    (void)size;
    if (va == (uintptr_t)opregion) {
        fw_unmaps++;
    }
}

/** Read the OpRegion's fixture into the firmware's memory; 0 when it is
 * not readable. */
static int opregion_load(void)
{
    long got = 0, r;
    int fd = open(LENOVO_OPREGION_FILE, 0);

    if (fd < 0) {
        return 0;
    }
    while (got < (long)sizeof(opregion) &&
           (r = read(fd, opregion + got, sizeof(opregion) - (unsigned long)got)) > 0) {
        got += r;
    }
    close(fd);
    return got == (long)sizeof(opregion);
}

/* DRM's mode helpers, which the panel's decoding calls and the bind does
 * not reach. */
uint32_t drm_mode_vrefresh(const struct drm_mode_modeinfo *mode)
{
    (void)mode;
    return 0;
}

void drm_mode_set_name(struct drm_mode_modeinfo *mode)
{
    mode->name[0] = '\0';
}

int pat_write_combining(void)
{
    return 1;
}

uint32_t kirq_save(void)
{
    irq_depth++;
    return 0;
}

void kirq_restore(uint32_t flags)
{
    (void)flags;
    irq_depth--;
}

/* The heap's pages: each a host page with a physical address of its own,
 * two pages apart from the last. */
#define TEST_PAGES 64
static struct {
    uint8_t *host;
    uint64_t phys;
} pages[TEST_PAGES];
static int npages, pages_live;
static uint64_t next_phys;

void *kalloc_pages(uint32_t count, uintptr_t *phys)
{
    void *block = 0;
    uint32_t i;

    if (npages + (int)count > TEST_PAGES ||
        posix_memalign(&block, 4096, (unsigned long)count * 4096u) != 0) {
        return 0;
    }
    for (i = 0; i < count; i++) {
        pages[npages].host = (uint8_t *)block + i * 4096u;
        pages[npages].phys = next_phys;
        next_phys += 0x2000u;
        npages++;
    }
    pages_live += (int)count;
    if (phys) {
        *phys = 0;
    }
    return block;
}

void kfree_pages(void *p, uint32_t count)
{
    int i;

    for (i = 0; i < npages; i++) {
        if (pages[i].host >= (uint8_t *)p && pages[i].host < (uint8_t *)p + count * 4096u) {
            pages[i].host = 0;
        }
    }
    pages_live -= (int)count;
    free(p);
}

void *kmalloc(size_t size)
{
    return malloc(size);
}

void *kzalloc(size_t size)
{
    return calloc(1, size);
}

void kfree(void *p)
{
    free(p);
}

uintptr_t paging_virt_to_phys(uintptr_t va)
{
    int i;

    for (i = 0; i < npages; i++) {
        if (pages[i].host && va >= (uintptr_t)pages[i].host &&
            va < (uintptr_t)pages[i].host + 4096u) {
            return (uintptr_t)(pages[i].phys + (va - (uintptr_t)pages[i].host));
        }
    }
    return 0;
}

/* The GTT's scratch page, a frame of its own. */
#define TEST_SCRATCH 0x7f000000u
static uint32_t scratch_page[1024];

uintptr_t alloc_frame(void)
{
    return TEST_SCRATCH;
}

void free_frame(uintptr_t phys)
{
    (void)phys;
}

int paging_zero_frame(uintptr_t phys)
{
    (void)phys;
    return 0;
}

/* Stolen memory lies outside the frames the allocator owns. */
int paging_frame_is_device(uintptr_t phys)
{
    (void)phys;
    return 1;
}

/** The model's way to memory: the host page at `phys`, or NULL, which
 * stops the ring that reached for it. */
static void *test_dma(struct i915_model *m, uint64_t phys)
{
    int i;

    (void)m;
    if (phys == TEST_SCRATCH) {
        return scratch_page;
    }
    for (i = 0; i < npages; i++) {
        if (pages[i].host && pages[i].phys == phys) {
            return pages[i].host;
        }
    }
    return 0;
}

void arch_dma_fence(void)
{}

/* No process waits at boot. */
struct percpu cpu_table[NR_CPUS];

void wait_queue_init(wait_queue_t *wq)
{
    wq->head = 0;
}

int wq_wait_event_nointr(wait_queue_t *wq, wq_cond_fn cond, void *arg)
{
    (void)wq;
    return cond(arg) ? 0 : -EINVAL;
}

void wq_wake_all(wait_queue_t *wq)
{
    (void)wq;
}

uint64_t rtc_ticks_now(void)
{
    return bus ? bus->now_us / 1000u : 0;
}

/* VT-d as intel_iommu=off leaves it: the DMAR table read, its RMRR for the
 * GPU named, and no unit translating. */
int iommu_present(void)
{
    return 0;
}

int iommu_unit_of(uint8_t b, uint8_t devfn)
{
    (void)b;
    (void)devfn;
    return -1;
}

int iommu_reserved_regions(uint8_t b, uint8_t devfn, struct iommu_region *out, int max)
{
    if (b != LENOVO_GPU_BUS || devfn != (LENOVO_GPU_DEVICE << 3) || max < 1) {
        return 0;
    }
    out[0].start = LENOVO_RMRR_BASE;
    out[0].end = LENOVO_RMRR_END;
    out[0].type = IOMMU_RESV_DIRECT;
    return 1;
}

/** Whether (bus, device, function) is the model's chip. */
static int is_chip(uint8_t b, uint8_t d, uint8_t f)
{
    return bus && b == LENOVO_GPU_BUS && d == LENOVO_GPU_DEVICE && f == LENOVO_GPU_FUNCTION;
}

uint32_t pci_read_config32(uint8_t b, uint8_t d, uint8_t f, uint8_t offset)
{
    return is_chip(b, d, f) ? i915_model_config(bus, offset, 4) : 0xffffffffu;
}

uint16_t pci_read_config16(uint8_t b, uint8_t d, uint8_t f, uint8_t offset)
{
    return is_chip(b, d, f) ? (uint16_t)i915_model_config(bus, offset, 2) : 0xffffu;
}

void pci_write_config16(uint8_t b, uint8_t d, uint8_t f, uint8_t offset, uint16_t value)
{
    if (is_chip(b, d, f)) {
        bus->config[offset] = (uint8_t)value;
        bus->config[offset + 1] = (uint8_t)(value >> 8);
    }
}

/** Set `bits` in the chip's command word. */
static void command_set(pci_device_t *dev, uint16_t bits)
{
    uint16_t c = pci_read_config16(dev->bus, dev->device, dev->function, PCI_COMMAND);

    pci_write_config16(dev->bus, dev->device, dev->function, PCI_COMMAND, (uint16_t)(c | bits));
}

void pci_enable_bus_master(pci_device_t *dev)
{
    command_set(dev, PCI_CMD_BUS_MASTER);
}

void pci_enable_memory_space(pci_device_t *dev)
{
    command_set(dev, PCI_CMD_MEMORY_SPACE);
}

bool pci_find_nth_by_id(uint16_t vendor_id, uint16_t device_id, int nth, pci_device_t *out)
{
    if (bus && nth == 0 && bus->pci.vendor_id == vendor_id && bus->pci.device_id == device_id) {
        *out = bus->pci;
        return true;
    }
    return false;
}

/** As pci.c's: the BAR's address, with the next BAR as its high half when
 * it is a 64-bit one. */
uint64_t pci_bar_address64(const pci_device_t *dev, int i)
{
    uint64_t phys;

    if (i < 0 || i > 5 || (dev->bar[i] & 1u)) {
        return 0;
    }
    phys = dev->bar[i] & ~0xfu;
    if ((dev->bar[i] & 6u) == 4u) {
        if (i == 5) {
            return 0;
        }
        phys |= (uint64_t)dev->bar[i + 1] << 32;
    }
    return phys;
}

int pci_msi_enable(const pci_device_t *dev, uint8_t vector)
{
    (void)vector;
    if (!is_chip(dev->bus, dev->device, dev->function)) {
        return -1;
    }
    bus->config[LENOVO_GPU_CAP_MSI + 2] |= 1u;
    return 0;
}

void pci_msi_disable(const pci_device_t *dev)
{
    if (is_chip(dev->bus, dev->device, dev->function)) {
        bus->config[LENOVO_GPU_CAP_MSI + 2] &= (uint8_t)~1u;
    }
}

int isr_msi_ready(void)
{
    return 1;
}

int isr_alloc_vector(void)
{
    return 48 + vectors++;
}

void isr_free_vector(int vector)
{
    (void)vector;
}

static isr_handler_t handler;
static uint8_t handler_vector;

void register_isr_handler(uint8_t vector, isr_handler_t h, const char *name)
{
    (void)name;
    handler = h;
    handler_vector = vector;
}

/* The display, intel_display.c and the files beside it, which have their
 * own test, i915_display_hosttest; here the display is made and adopts
 * nothing. */
int32_t intel_display_init(struct drm_i915_private *i915)
{
    (void)i915;
    return 0;
}

void intel_display_fini(struct drm_i915_private *i915)
{
    (void)i915;
}

void drm_mode_config_cleanup(struct drm_device *dev)
{
    (void)dev;
}

int32_t drm_aperture_remove_conflicting(uint64_t base, uint64_t size, const char *name)
{
    (void)base;
    (void)size;
    (void)name;
    return 0;
}

int32_t i915_gem_dumb_create(struct drm_file *file, struct drm_device *dev,
                             struct drm_mode_create_dumb *args)
{
    (void)dev;
    (void)file;
    (void)args;
    return -ENODEV;
}

void drm_dev_init(struct drm_device *dev, const struct drm_driver *driver)
{
    k_memset(dev, 0, sizeof(*dev));
    dev->driver = driver;
    dev->features = driver->ident.features;
    dev->index = -1;
    dev->refs = 1;
}

int32_t drm_dev_register(struct drm_device *dev)
{
    last_registered = dev;
    dev->registered = 1;
    dev->index = 0;
    k_strncpy(dev->unique, "pci:0000:00:02.0", sizeof(dev->unique) - 1);
    return 0;
}

/* The driver's buffers, i915_gem.c and the files beside it, which the bind
 * reaches only through the DRM core's hooks and the unplug. */
const struct drm_ioctl_desc i915_ioctls[I915_NUM_IOCTLS];

struct drm_gem_object *i915_gem_prime_import(struct drm_device *dev, struct dma_buf *buf,
                                             int32_t *err)
{
    (void)dev;
    (void)buf;
    *err = -ENODEV;
    return 0;
}

void i915_gem_unplug(struct drm_i915_private *i915)
{
    (void)i915;
}

void i915_gem_shrinker_register(struct drm_i915_private *i915)
{
    (void)i915;
}

void i915_gem_shrinker_unregister(struct drm_i915_private *i915)
{
    (void)i915;
}

void i915_debugfs_register(struct drm_i915_private *i915)
{
    (void)i915;
}

/* The contexts and requests, i915_gem_context.c and i915_request.c, which
 * the bind reaches only through the DRM core's hooks and the unplug. */
int32_t i915_driver_open(struct drm_device *dev, struct drm_file *file)
{
    (void)dev;
    (void)file;
    return 0;
}

void i915_driver_postclose(struct drm_device *dev, struct drm_file *file)
{
    (void)dev;
    (void)file;
}

void i915_requests_unplug(struct drm_i915_private *i915)
{
    (void)i915;
}

/* A page of a backend that stands in for the aperture, which the chip's
 * backend here has not. */
void *kmap_temp(uintptr_t phys)
{
    (void)phys;
    return 0;
}

void kunmap_temp(void *va)
{
    (void)va;
}

void drm_dev_put(struct drm_device *dev)
{
    if (--dev->refs == 0 && dev->driver->release) {
        dev->driver->release(dev);
    }
}

/* ---- the boundary the driver sees ---- */

/* The model's own accesses, which the test's pass each access on to. */
static const struct i915_hw_ops *model_ops;

/* Each register below the GTT that the driver wrote, by dword; the writes
 * that reached the firmware's framebuffer entries; the writes that
 * withdrew the driver's request for the power well. */
static uint8_t written[I915_GSM_OFFSET / 4];
static uint32_t kept_writes, well_drops;

/** A read, passed on to the model. */
static uint32_t trial_read(struct i915_hw *hw, uint32_t reg)
{
    return model_ops->read32(hw, reg);
}

/** A write, noted, then passed on to the model. */
static void trial_write(struct i915_hw *hw, uint32_t reg, uint32_t value)
{
    if (reg < I915_GSM_OFFSET) {
        written[reg / 4] = 1;
    } else if ((reg - I915_GSM_OFFSET) / 4u < LENOVO_FW_FB_BYTES / 4096u) {
        kept_writes++;
    }
    if (reg == HSW_PWR_WELL_CTL2 && !(value & HSW_PWR_WELL_CTL_REQ(HSW_PW_CTL_IDX_GLOBAL))) {
        well_drops++;
    }
    model_ops->write32(hw, reg, value);
}

/** A wait, passed on to the model, whose clock moves. */
static void trial_delay(struct i915_hw *hw, uint32_t us)
{
    model_ops->delay_us(hw, us);
}

static const struct i915_hw_ops trial_ops = {"trial", trial_read, trial_write, trial_delay, 0};

/** The model's clock moved: the processor takes the device's messages,
 * while its interrupts are on. */
static void on_delay(struct i915_model *m)
{
    isr_params_t params;

    if (!irq_on || irq_depth != 0 || !handler) {
        return;
    }
    while (i915_model_take_message(m)) {
        k_memset(&params, 0, sizeof(params));
        params.vector = handler_vector;
        handler(&params);
    }
}

/* ---- rows ---- */

/* The registers the bind may write besides forcewake and the rings':
 * outside the GT, the unclaimed access and FIFO errors cleared, the GTT's
 * flush, every interrupt bank and the error and self refresh interrupts,
 * the power well's takeover from the BIOS and the pcode mailbox; in the
 * GT, RC6's and RPS's registers, as Linux's gen6_rc6_enable and
 * gen6_rps_enable write them, with the idle limits of the video and
 * video enhancement engines, inside their rings' registers too; and
 * the swizzle of tiles, in the display's arbiter, the GT's tile control
 * and ARB_MODE, as Linux's intel_gt_init_swizzling sets it on the
 * Lenovo, whose firmware's surface is linear; and the PPGTT's caching, as
 * Linux's gen7_ppgtt_enable sets it. The fence registers, which the bind
 * clears, are `write_ok`'s. */
static const uint32_t writes_ok[] = {GEN6_PCODE_MAILBOX,
                                     GEN6_PCODE_DATA,
                                     GEN6_PCODE_DATA1,
                                     GEN6_PMINTRMSK,
                                     GEN6_RC1_WAKE_RATE_LIMIT,
                                     GEN6_RC1e_THRESHOLD,
                                     GEN6_RC6_THRESHOLD,
                                     GEN6_RC6_WAKE_RATE_LIMIT,
                                     GEN6_RC6p_THRESHOLD,
                                     GEN6_RC6pp_THRESHOLD,
                                     GEN6_RC6pp_WAKE_RATE_LIMIT,
                                     GEN6_RC_CONTROL,
                                     GEN6_RC_EVALUATION_INTERVAL,
                                     GEN6_RC_IDLE_HYSTERSIS,
                                     GEN6_RC_SLEEP,
                                     GEN6_RC_STATE,
                                     GEN6_RPNSWREQ,
                                     GEN6_RP_CONTROL,
                                     GEN6_RP_DOWN_EI,
                                     GEN6_RP_DOWN_THRESHOLD,
                                     GEN6_RP_DOWN_TIMEOUT,
                                     GEN6_RP_IDLE_HYSTERSIS,
                                     GEN6_RP_INTERRUPT_LIMITS,
                                     GEN6_RP_UP_EI,
                                     GEN6_RP_UP_THRESHOLD,
                                     RING_MAX_IDLE(GEN6_BSD_RING_BASE),
                                     RING_MAX_IDLE(VEBOX_RING_BASE),
                                     FPGA_DBG,
                                     GTFIFODBG,
                                     GFX_FLSH_CNTL_GEN6,
                                     DEIMR,
                                     DEIIR,
                                     DEIER,
                                     GTIMR,
                                     GTIIR,
                                     GTIER,
                                     GEN6_PMIMR,
                                     GEN6_PMIIR,
                                     GEN6_PMIER,
                                     GEN7_ERR_INT,
                                     EDP_PSR_IMR,
                                     EDP_PSR_IIR,
                                     SDEIMR,
                                     SDEIIR,
                                     SDEIER,
                                     SERR_INT,
                                     HSW_PWR_WELL_CTL1,
                                     HSW_PWR_WELL_CTL2,
                                     DISP_ARB_CTL,
                                     TILECTL,
                                     ARB_MODE,
                                     GAM_ECOCHK,
                                     GAC_ECO_BITS};

/** Whether `reg`, written, belongs to what the bind may write: the GT's
 * forcewake, the fence registers, the four rings and their status page
 * registers, or a register of `writes_ok`, RC6's and RPS's among them. */
static int write_ok(uint32_t reg)
{
    unsigned i;

    if (reg == FORCEWAKE_MT || reg == RENDER_HWS_PGA_GEN7 || reg == BLT_HWS_PGA_GEN7 ||
        reg == BSD_HWS_PGA_GEN7 || reg == VEBOX_HWS_PGA_GEN7 ||
        (reg >= FENCE_REG_GEN6_LO(0) && reg < FENCE_REG_GEN6_LO(I915_NUM_FENCES)) ||
        (reg >= RENDER_RING_BASE && reg < RENDER_RING_BASE + 0x1000u) ||
        (reg >= BLT_RING_BASE && reg < BLT_RING_BASE + 0x1000u) ||
        (reg >= GEN6_BSD_RING_BASE && reg < GEN6_BSD_RING_BASE + 0x1000u) ||
        (reg >= VEBOX_RING_BASE && reg < VEBOX_RING_BASE + 0x1000u)) {
        return 1;
    }
    for (i = 0; i < sizeof(writes_ok) / sizeof(writes_ok[0]); i++) {
        if (reg == writes_ok[i]) {
            return 1;
        }
    }
    return 0;
}

/** Check that the bind left the firmware's display as it was: no
 * register written but what `write_ok` allows, the framebuffer's GTT
 * entries, plane, pipe and eDP route untouched, the rest of stolen memory
 * unmapped, and the power well held. */
static void check_display(struct i915_model *m, struct drm_i915_private *i915)
{
    uint32_t reg, i, bad = 0, fw_ok = 1, cleared_ok = 1;

    for (reg = 0; reg < I915_GSM_OFFSET; reg += 4) {
        if (written[reg / 4] && !write_ok(reg)) {
            printf("i915-trial-test: register %06x written\n", (unsigned)reg);
            bad++;
        }
    }
    CHECK("display: no register written but the interrupts', the flush's, the well's and power "
          "management's",
          !bad);
    CHECK("display: no write reached the firmware's framebuffer entries", kept_writes == 0);
    for (i = 0; i < 16384u; i++) {
        uint32_t pte = i915_model_peek(m, I915_GSM_OFFSET + i * 4u);
        if (i < LENOVO_FW_FB_BYTES / 4096u) {
            fw_ok &= pte == (HSW_GTT_ADDR_ENCODE(LENOVO_STOLEN_BASE + i * 4096u) | GEN6_PTE_VALID);
        } else if (i < (uint32_t)(i915->gt.ggtt_bottom / 4096u)) {
            cleared_ok &= pte == i915->ggtt.scratch_pte;
        }
    }
    CHECK("display: the framebuffer's 1032 entries as the firmware left them", fw_ok);
    CHECK("display: the rest of stolen memory unmapped, to the scratch page", cleared_ok);
    CHECK("display: the plane, pipe and eDP route as the firmware left them",
          i915_model_peek(m, DSPCNTR(I915_PIPE_A)) == (DISP_ENABLE | DISP_FORMAT_BGRX888) &&
              i915_model_peek(m, DSPSURF(I915_PIPE_A)) == LENOVO_FW_FB_GGTT &&
              i915_model_peek(m, DSPSTRIDE(I915_PIPE_A)) == LENOVO_FW_FB_STRIDE &&
              i915_model_peek(m, TRANS_DDI_FUNC_CTL(I915_TRANS_EDP)) == LENOVO_FW_EDP_DDI_FUNC &&
              i915_model_peek(m, TRANSCONF(I915_TRANS_EDP)) == LENOVO_FW_EDP_TRANSCONF);
    CHECK("power: the well never released, and on",
          well_drops == 0 && i915_model_power(m)->on && i915->power.count == 1);
}

/** Check that every interrupt source of `m` is masked, only the master
 * enable and the rings' user interrupts are enabled, and the firmware's
 * unclaimed access is cleared. */
static void check_interrupts(struct i915_model *m)
{
    CHECK("interrupts: every source masked",
          i915_model_peek(m, GTIMR) == 0xffffffffu && i915_model_peek(m, DEIMR) == 0xffffffffu &&
              i915_model_peek(m, GEN6_PMIMR) == 0xffffffffu &&
              i915_model_peek(m, SDEIMR) == 0xffffffffu &&
              i915_model_peek(m, RING_IMR(RENDER_RING_BASE)) == 0xffffffffu &&
              i915_model_peek(m, RING_IMR(BLT_RING_BASE)) == 0xffffffffu &&
              i915_model_peek(m, RING_IMR(GEN6_BSD_RING_BASE)) == 0xffffffffu &&
              i915_model_peek(m, RING_IMR(VEBOX_RING_BASE)) == 0xffffffffu);
    CHECK("interrupts: only the master enable and the rings' user interrupts enabled",
          i915_model_peek(m, DEIER) == DE_MASTER_IRQ_CONTROL &&
              i915_model_peek(m, GTIER) ==
                  (GT_RENDER_USER_INTERRUPT | GT_BLT_USER_INTERRUPT | GT_BSD_USER_INTERRUPT) &&
              i915_model_peek(m, GEN6_PMIER) == PM_VEBOX_USER_INTERRUPT &&
              i915_model_peek(m, SDEIER) == 0);
    CHECK("interrupts: the firmware's unclaimed access cleared, no error latched",
          !(i915_model_peek(m, FPGA_DBG) & FPGA_DBG_RM_NOCLAIM) &&
              i915_model_peek(m, GEN7_ERR_INT) == 0);
}

/** Put a fresh model of the Lenovo's chip on the bus, seen through the
 * test's boundary `hw`, with the boot options of one of the trial's boots
 * and the processor's interrupts on or off, the kernel's records cleared. */
static void fresh(struct i915_model *m, struct i915_hw *hw, int firstlight, int interrupts)
{
    CHECK("the model is made", i915_model_init(m) == 0);
    bus = m;
    m->on_delay = on_delay;
    i915_model_irq_state(m)->dma = test_dma;
    model_ops = m->hw.ops;
    *hw = m->hw;
    hw->ops = &trial_ops;
    k_memset(written, 0, sizeof(written));
    kept_writes = well_drops = 0;
    log_text[0] = 0;
    irq_on = interrupts;
    irq_depth = 0;
    npages = pages_live = 0;
    next_phys = 0x50000000ull;
    last_registered = 0;
    handler = 0;
    opt_i915 = 1;
    opt_firstlight = firstlight;
    CHECK("the OpRegion is read", opregion_load());
    fw_maps = fw_unmaps = 0;
}

/**
 * Bind the driver to a fresh model of the Lenovo's chip with the boot
 * options of one of the trial's boots, the processor's interrupts on or
 * off, and check what the trial must leave as it was.
 */
static void run(int firstlight, int interrupts)
{
    struct i915_model m;
    struct drm_i915_private *i915;
    struct i915_hw hw;
    struct i915_model_ring *rcs, *bcs;

    fresh(&m, &hw, firstlight, interrupts);
    CHECK("bind: the driver binds", i915_driver_bind(&m.pci, &hw) == 0 && last_registered);
    if (last_registered) {
        i915 = to_i915(last_registered);
        rcs = i915_model_ring_state(&m, 0);
        bcs = i915_model_ring_state(&m, 1);
        CHECK("bind: no wait reached its bound", i915->hw.timeouts == 0);
        CHECK("bind: registered, the rings started",
              logged("registered: i915 1.6.0") &&
                  logged("rings: rcs0, bcs0, vcs0 and vecs0 started") &&
                  i915->gt.engine[I915_RCS0].running && i915->gt.engine[I915_BCS0].running &&
                  i915->gt.engine[I915_VCS0].running && i915->gt.engine[I915_VECS0].running);
        CHECK("bind: stolen memory inside the RMRR",
              i915->stolen.size == 0x4000000u && i915->stolen.in_rmrr);
        CHECK("bind: the VBT from the OpRegion's mailbox 4, three outputs",
              fw_maps == 1 && i915->vbt.present && i915->vbt.version == LENOVO_VBT_VERSION &&
                  i915->vbt.nencoders == 3 && logged("VBT in mailbox 4"));
        check_display(&m, i915);
        check_interrupts(&m);
        CHECK("gpu: no memory reached but the driver's pages", !rcs->faulted && !bcs->faulted);
        CHECK("gpu: nothing run on the blitter", bcs->executed == 0);
        if (firstlight) {
            CHECK("light: ok, the interrupt taken or latched",
                  logged("first light: ok: rcs0 wrote 0x1915f157") &&
                      logged(interrupts ? "user interrupt taken" : "user interrupt latched"));
            CHECK("light: the value page given back",
                  i915->gt.engine[I915_RCS0].irq_refs == 0 && pages_live == 10);
        } else {
            CHECK("bind: no first light without its option",
                  !logged("first light") && rcs->executed == 0 &&
                      i915->gt.engine[I915_RCS0].tail == 0);
        }
        i915_unplug(last_registered);
        drm_dev_put(last_registered);
        last_registered = 0;
        CHECK("unplug: every page given back", pages_live == 0);
        CHECK("release: the OpRegion unmapped", fw_unmaps == 1);
    }
    i915_model_free(&m);
    bus = 0;
}

int main(void)
{
    run(0, 1);
    run(0, 0);
    run(1, 1);
    run(1, 0);
    if (fails) {
        printf("i915-trial-test: %d failed\n", fails);
        return 1;
    }
    printf("i915-trial-test: ok\n");
    return 0;
}
