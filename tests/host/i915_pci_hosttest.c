/**
 * i915_pci_hosttest.c: the Intel graphics driver's binding,
 * rach2/sys/drm/i915/i915_pci.c, and its hardware boundary, i915_hw.c,
 * against the host model of the Lenovo's HD 4400.
 *
 * host-deps: rach2/sys/drm/i915/i915_hw.c tests/host/i915_model.c
 * host-deps: rach2/sys/drm/i915/i915_uncore.c rach2/sys/drm/i915/i915_power.c
 * host-deps: rach2/sys/drm/i915/i915_stolen.c rach2/sys/drm/i915/i915_gtt.c
 * host-deps: tests/host/compat/kheap_host.c rach2/sys/lib/kstring.c
 * host-deps: rach2/sys/lib/kctypes.c rach2/sys/mm/frame.c
 * host-deps: tests/host/compat/spinlock_host.c rach2/sys/drm/i915/i915_irq.c
 * host-deps: rach2/sys/drm/i915/i915_ring.c rach2/sys/drm/i915/i915_ppgtt.c
 * host-deps: rach2/sys/drm/i915/i915_firstlight.c rach2/sys/drm/i915/i915_rc6.c
 * host-deps: rach2/sys/drm/i915/i915_rps.c rach2/sys/tick.c
 * host-deps: rach2/sys/drm/i915/intel_bios.c
 * host-deps: rach2/sys/drm/i915/intel_opregion.c
 * host-deps: rach2/sys/drm/i915/i915_aperture.c rach2/sys/drm/i915/i915_fence.c
 *
 * i915_pci.c is included whole. The bus holds the model's 00:02.0, whose
 * configuration space the PCI calls read and write; the DRM core is a
 * record of the device registered; a message vector is a number handed
 * out in order. A row binds the driver to the model through
 * `i915_driver_bind`, as the fault backend will, or probes the bus, which
 * maps BAR 0 through the MMIO backend; `map_mmio_typed` then hands out a
 * buffer of the test's in place of the chip's registers, and another in
 * place of the GTT's entries. The firmware's memory cannot be mapped, so
 * the bind finds no OpRegion and no option ROM and takes Linux's defaults
 * for the outputs; intel_bios_hosttest.c tests the tables themselves. The
 * bind's base, forcewake, the power well, stolen memory and the GTT, is
 * i915_base_hosttest.c's, and its rings and interrupts
 * i915_ring_hosttest.c's; here the frame allocator hands out
 * one frame, the heap's pages have made-up physical addresses, and the
 * IOMMU is off. RC6 and the GT's frequency are i915_power_hosttest.c's,
 * and the card has no directory in /sys here.
 *
 * The rows: the id table against Linux 6.12's Haswell GT2 ids; the probe
 * without and with rach2.i915; the bind to the model, with every source
 * masked and cleared before the message is enabled and only the master
 * enable and the rings' user interrupts enabled after, the configuration
 * words read, the command word's decoding and mastering on, and the DRM
 * device's identity and features; the refusals of another function, an
 * unset BAR 0, another vendor and an unknown id; a machine without
 * messages and a message that will not enable; a registration that fails
 * and leaves the device as found; the unplug hook; and the boundary's
 * accesses past its size and its bounded waits on the model's clock.
 *
 * Returns 0 when every row passes, 1 otherwise.
 */

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

extern int printf(const char *format, ...);
extern void *calloc(unsigned long count, unsigned long size);
extern void free(void *p);
extern int strcmp(const char *a, const char *b);

static int fails;

#define CHECK(what, cond)                              \
    do {                                               \
        if (!(cond)) {                                 \
            printf("i915-pci-test: fail: %s\n", what); \
            fails++;                                   \
        }                                              \
    } while (0)

/* ---- the kernel around the driver ---- */

static struct i915_model *bus; /* the chip on the bus, or NULL */
static int cmdline_i915;       /* rach2.i915 given */
static int msi_ready = 1, msi_refuse, register_error;
static int vectors, vectors_freed, handlers, msi_enables, msi_disables;
static long msi_enabled_at = -1;
static int registered, released;
static struct drm_device *last_registered;
static uintptr_t map_phys;
static size_t map_size;
static pte_t map_cache;
static int maps;
static uint32_t *map_buffer, *gsm_buffer;
static pte_t gsm_cache;
static uint32_t udelays;
static int frames, frames_freed;

/** The boot log: nothing to keep. */
void klog(int level, const char *subsystem, const char *format, ...)
{
    (void)level;
    (void)subsystem;
    (void)format;
}

int kcmdline_has(const char *flag)
{
    return cmdline_i915 && strcmp(flag, "rach2.i915") == 0;
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

int sysfs_pci_bind(int key, const char *driver)
{
    (void)key;
    (void)driver;
    return 0;
}

void sysfs_pci_unbind(int key)
{
    (void)key;
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

/** The fake chip, which i915_fake_hosttest.c tests: no command line here
 * names it, so the probe never runs. */
int32_t i915_fake_probe(void)
{
    return 0;
}

void k_udelay(uint32_t usec)
{
    udelays += usec;
}

/** BAR 0's registers in `map_buffer`, and the GTT's entries, BAR 0's
 * second half, in `gsm_buffer`. */
uintptr_t map_mmio_typed(uintptr_t phys_addr, size_t size, pte_t cache)
{
    maps++;
    if (phys_addr == LENOVO_GPU_BAR0 + I915_GSM_OFFSET) {
        gsm_cache = cache;
        free(gsm_buffer);
        gsm_buffer = (uint32_t *)calloc(size / 4, sizeof(uint32_t));
        return (uintptr_t)gsm_buffer;
    }
    map_phys = phys_addr;
    map_size = size;
    map_cache = cache;
    free(map_buffer);
    map_buffer = (uint32_t *)calloc(size / 4, sizeof(uint32_t));
    return (uintptr_t)map_buffer;
}

/* The firmware's memory, the OpRegion at ASLS and the option ROM's
 * shadow, which this test does not give: each map is counted and fails. */
static int fw_maps;

uintptr_t mmio_map(uint64_t phys, size_t size, pte_t cache)
{
    (void)phys;
    (void)size;
    (void)cache;
    fw_maps++;
    return 0;
}

void mmio_unmap(uintptr_t va, size_t size)
{
    (void)va;
    (void)size;
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
    return 0;
}

void kirq_restore(uint32_t flags)
{
    (void)flags;
}

uintptr_t alloc_frame(void)
{
    frames++;
    return 0x12345000u;
}

void free_frame(uintptr_t phys)
{
    (void)phys;
    frames_freed++;
}

int paging_zero_frame(uintptr_t phys)
{
    (void)phys;
    return 0;
}

int paging_frame_is_device(uintptr_t phys)
{
    (void)phys;
    return 1;
}

int iommu_present(void)
{
    return 0;
}

/** A made-up physical page for the heap's address `va`, never 0; the
 * host's heap does not align its pages, so the offset is dropped. */
uintptr_t paging_virt_to_phys(uintptr_t va)
{
    return (uintptr_t)(0x60000000u + ((va >> 12) & 0xffffu) * 4096u);
}

void arch_dma_fence(void)
{}

/* No process waits on a ring in these rows. */
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
    (void)b;
    (void)devfn;
    (void)out;
    (void)max;
    return 0;
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
    if (msi_refuse || !is_chip(dev->bus, dev->device, dev->function)) {
        return -1;
    }
    msi_enables++;
    msi_enabled_at = (long)bus->nlog;
    bus->config[LENOVO_GPU_CAP_MSI + 2] |= 1u;
    return 0;
}

void pci_msi_disable(const pci_device_t *dev)
{
    msi_disables++;
    if (is_chip(dev->bus, dev->device, dev->function)) {
        bus->config[LENOVO_GPU_CAP_MSI + 2] &= (uint8_t)~1u;
    }
}

int isr_msi_ready(void)
{
    return msi_ready;
}

int isr_alloc_vector(void)
{
    return 48 + vectors++;
}

void isr_free_vector(int vector)
{
    (void)vector;
    vectors_freed++;
}

static isr_handler_t handler;

void register_isr_handler(uint8_t vector, isr_handler_t h, const char *name)
{
    (void)vector;
    (void)name;
    handlers++;
    handler = h;
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
    if (register_error) {
        return register_error;
    }
    registered++;
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

/* The release and the bring-up through /sys, i915_handoff.c, which
 * i915_handoff_hosttest.c tests; no row here unbinds the driver. */
void i915_handoff_bound(struct drm_i915_private *i915)
{
    (void)i915;
}

void i915_handoff_unbound(struct drm_i915_private *i915)
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
        released++;
        dev->driver->release(dev);
    }
}

/* ---- rows ---- */

/** Put a fresh model of the Lenovo's chip on the bus and clear the
 * kernel's records. */
static void fresh(struct i915_model *m)
{
    CHECK("the model is made", i915_model_init(m) == 0);
    bus = m;
    msi_ready = 1;
    msi_refuse = 0;
    register_error = 0;
    vectors = vectors_freed = handlers = msi_enables = msi_disables = 0;
    msi_enabled_at = -1;
    registered = released = 0;
    last_registered = 0;
    maps = 0;
    udelays = 0;
    frames = frames_freed = 0;
    cmdline_i915 = 0;
}

/** Unplug and drop what a row bound, and take the model off the bus. */
static void done(struct i915_model *m)
{
    if (last_registered) {
        i915_unplug(last_registered);
        drm_dev_put(last_registered);
        last_registered = 0;
    }
    i915_model_free(m);
    bus = 0;
}

/** The id table: Linux 6.12's Haswell GT2 ids and hsw_gt2_info. */
static void row_ids(void)
{
    static const uint16_t gt2[] = {0x0A12, 0x0A16, 0x0A1A, 0x0A1B, 0x0A1E, 0x0412, 0x0416,
                                   0x041A, 0x041B, 0x041E, 0x0C12, 0x0C16, 0x0C1A, 0x0C1B,
                                   0x0C1E, 0x0D12, 0x0D16, 0x0D1A, 0x0D1B, 0x0D1E};
    unsigned i;

    CHECK("ids: twenty, Linux's Haswell GT2 ids", i915_nids == 20);
    for (i = 0; i < sizeof(gt2) / sizeof(gt2[0]); i++) {
        const struct i915_id *id = i915_id_lookup(gt2[i]);
        CHECK("ids: each GT2 id is bound", id && id->platform->gt == 2);
        CHECK("ids: Haswell, graphics 7.5, display 7",
              id && strcmp(id->platform->name, "HASWELL") == 0 && id->platform->graphics_ver == 7 &&
                  id->platform->graphics_rel == 50 && id->platform->display_ver == 7);
    }
    CHECK("ids: 0a16 is ULT and not ULX",
          i915_id_lookup(0x0A16)->ult && !i915_id_lookup(0x0A16)->ulx);
    CHECK("ids: 0a1e is ULX and ULT", i915_id_lookup(0x0A1E)->ult && i915_id_lookup(0x0A1E)->ulx);
    CHECK("ids: 0416 is neither", !i915_id_lookup(0x0416)->ult && !i915_id_lookup(0x0416)->ulx);
    CHECK("ids: GT1 0a06 is not this driver's", !i915_id_lookup(0x0A06));
    CHECK("ids: GT3 0a26 is not this driver's", !i915_id_lookup(0x0A26));
    CHECK("ids: Ivy Bridge 0166 is not this driver's", !i915_id_lookup(0x0166));
    CHECK("ids: hsw_gt2_info's features",
          i915_ids[0].platform->has_llc && i915_ids[0].platform->has_l3_dpf &&
              i915_ids[0].platform->has_rc6 && !i915_ids[0].platform->has_rc6p &&
              i915_ids[0].platform->has_reset_engine && i915_ids[0].platform->ppgtt_size == 31 &&
              i915_ids[0].platform->dma_mask_size == 40 &&
              i915_ids[0].platform->engines ==
                  (I915_ENGINE_RCS0 | I915_ENGINE_BCS0 | I915_ENGINE_VCS0 | I915_ENGINE_VECS0));
}

/** The probe without rach2.i915, and on a bus without the chip. */
static void row_probe_gated(void)
{
    struct i915_model m;

    fresh(&m);
    CHECK("gated: no device without rach2.i915", i915_probe() == -ENODEV);
    CHECK("gated: nothing mapped", maps == 0);
    CHECK("gated: nothing registered", registered == 0);
    CHECK("gated: the command word untouched", i915_model_config(&m, PCI_COMMAND, 2) == 0x0407u);
    done(&m);

    fresh(&m);
    bus = 0;
    cmdline_i915 = 1;
    CHECK("gated: no device on an empty bus", i915_probe() == -ENODEV);
    done(&m);
}

/** The probe with rach2.i915, through the MMIO backend. */
static void row_probe_mmio(void)
{
    struct i915_model m;
    struct drm_i915_private *i915;

    fresh(&m);
    cmdline_i915 = 1;
    CHECK("probe: one device bound", i915_probe() == 1);
    CHECK("probe: BAR 0's registers and the GTT's entries mapped, once each", maps == 2);
    CHECK("probe: at 0xc0000000", map_phys == LENOVO_GPU_BAR0);
    CHECK("probe: the registers' 2 MiB", map_size == I915_MMIO_SIZE);
    CHECK("probe: uncached", map_cache == PTE_NOCACHE);
    CHECK("probe: the GTT's entries write-combining", gsm_cache == PTE_WC);
    CHECK("probe: registered", registered == 1 && last_registered);
    i915 = last_registered ? to_i915(last_registered) : 0;
    CHECK("probe: through the MMIO backend", i915 && strcmp(i915->hw.ops->name, "mmio") == 0 &&
                                                 i915->hw.priv == (void *)map_buffer &&
                                                 i915->hw.size == I915_MMIO_SIZE);
    CHECK("probe: the display engine's interrupts masked and off but the master enable",
          map_buffer[DEIMR / 4] == 0xffffffffu && map_buffer[DEIER / 4] == DE_MASTER_IRQ_CONTROL);
    CHECK("probe: the south display's masked and off",
          map_buffer[SDEIMR / 4] == 0xffffffffu && map_buffer[SDEIER / 4] == 0);
    CHECK("probe: no register reached the model", m.nlog == 0);
    done(&m);
}

/** The log index of the write of `value` to `reg`, or a large number. */
static long at(const struct i915_model *m, uint32_t reg, uint32_t value)
{
    long i = i915_model_find_write(m, reg, value, 0);
    return i < 0 ? 1L << 30 : i;
}

/** The interrupt registers of each source: mask, enable and identity; the
 * enable the bind leaves: the display engine's master enable, and the user
 * interrupts of the rings it started, each still masked; and the times the
 * identity is cleared: twice by the reset, and the power management's
 * twice more by RPS's own reset, as Linux's gen6_rps_reset_interrupts. */
static const uint32_t banks[][5] = {
    {DEIMR, DEIER, DEIIR, DE_MASTER_IRQ_CONTROL, 2},
    {GTIMR, GTIER, GTIIR, GT_RENDER_USER_INTERRUPT | GT_BLT_USER_INTERRUPT | GT_BSD_USER_INTERRUPT,
     2},
    {GEN6_PMIMR, GEN6_PMIER, GEN6_PMIIR, PM_VEBOX_USER_INTERRUPT, 4},
    {SDEIMR, SDEIER, SDEIIR, 0, 2}};

/**
 * Check that the bind masked, disabled and cleared every source of `m`, in
 * that order, before it enabled the message, and before its base, which
 * starts with forcewake's reset, read no register but by the posting reads
 * of those writes.
 */
static void check_masks(struct i915_model *m)
{
    uint32_t posting = 0, reads = 0, i;
    long base = -1;

    for (i = 0; i < m->nlog && base < 0; i++) {
        if (m->log[i].reg == FPGA_DBG) {
            base = (long)i;
        } else if (m->log[i].op == I915_MODEL_READ) {
            reads++;
        }
    }
    CHECK("bind: the base after the masks", base > at(m, SERR_INT, 0xffffffffu));

    for (i = 0; i < (uint32_t)(sizeof(banks) / sizeof(banks[0])); i++) {
        long mask = at(m, banks[i][0], 0xffffffffu);
        long off = at(m, banks[i][1], 0);
        long clear = at(m, banks[i][2], 0xffffffffu);
        CHECK("bind: each bank masked, then off, then cleared", mask < off && off < clear);
        CHECK("bind: each bank's identity cleared as often as on Linux",
              i915_model_count(m, I915_MODEL_WRITE, banks[i][2]) == banks[i][4]);
        CHECK("bind: each bank masked now, and off but the master and the rings",
              i915_model_peek(m, banks[i][0]) == 0xffffffffu &&
                  i915_model_peek(m, banks[i][1]) == banks[i][3]);
        CHECK("bind: each source masked before the message", msi_enabled_at > clear);
        posting += i915_model_count(m, I915_MODEL_READ, banks[i][0]) +
                   i915_model_count(m, I915_MODEL_READ, banks[i][2]);
    }
    CHECK("bind: the error interrupt cleared", at(m, GEN7_ERR_INT, 0xffffffffu) < msi_enabled_at);
    CHECK("bind: the self refresh's masked and cleared",
          at(m, EDP_PSR_IMR, 0xffffffffu) < msi_enabled_at &&
              at(m, EDP_PSR_IIR, 0xffffffffu) < msi_enabled_at);
    CHECK("bind: the south errors cleared", at(m, SERR_INT, 0xffffffffu) < msi_enabled_at);
    CHECK("bind: no register read before the base but by the posting reads", reads == posting);
}

/** Check the configuration words the bind of `m` read and wrote. */
static void check_config(struct i915_model *m, const struct drm_i915_private *i915)
{
    uint32_t gms = (i915->gmch_ctrl >> SNB_GMCH_GMS_SHIFT) & SNB_GMCH_GMS_MASK;
    uint32_t stolen = i915->bsm & INTEL_BSM_MASK;
    uint32_t command = i915_model_config(m, PCI_COMMAND, 2);

    CHECK("bind: the message on, once, with a handler",
          msi_enables == 1 && handlers == 1 && i915->msi_vector == 48);
    CHECK("bind: the message's enable bit set", (m->config[LENOVO_GPU_CAP_MSI + 2] & 1u) != 0);
    CHECK("bind: memory decoding and bus mastering on",
          (command & (PCI_CMD_MEMORY_SPACE | PCI_CMD_BUS_MASTER)) ==
              (PCI_CMD_MEMORY_SPACE | PCI_CMD_BUS_MASTER));
    CHECK("bind: INTx stays disabled", (command & PCI_CMD_INTERRUPT_DIS) != 0);
    CHECK("bind: the BARs",
          i915->mmio_phys == LENOVO_GPU_BAR0 && i915->gmadr_phys == LENOVO_GPU_BAR2);
    CHECK("bind: the board's ids", i915->subsystem_vendor == LENOVO_GPU_SUBVENDOR &&
                                       i915->subsystem_device == LENOVO_GPU_SUBDEVICE);
    CHECK("bind: the GMCH word and BDSM as the firmware left them",
          i915->gmch_ctrl == LENOVO_GPU_GGC && i915->bsm == LENOVO_GPU_BDSM);
    CHECK("bind: the stolen memory is Linux's reservation",
          stolen == LENOVO_STOLEN_BASE && stolen + gms * 0x2000000u - 1 == LENOVO_STOLEN_END);
    CHECK("bind: the stolen memory inside the RMRR",
          stolen >= LENOVO_RMRR_BASE && stolen + gms * 0x2000000u - 1 <= LENOVO_RMRR_END);
    CHECK("bind: a 2 MiB GTT",
          ((i915->gmch_ctrl >> SNB_GMCH_GGMS_SHIFT) & SNB_GMCH_GGMS_MASK) == 2);
}

/** Check the DRM device's identity and features, Linux's i915 with the
 * display made. */
static void check_identity(const struct drm_device *dev, const struct drm_i915_private *i915)
{
    const struct drm_ident *id = &dev->driver->ident;

    CHECK("identity: i915", strcmp(id->name, "i915") == 0);
    CHECK("identity: Intel Graphics", strcmp(id->desc, "Intel Graphics") == 0);
    CHECK("identity: 1.6.0 of 20230929", id->major == 1 && id->minor == 6 && id->patchlevel == 0 &&
                                             strcmp(id->date, "20230929") == 0);
    CHECK("identity: on the PCI bus", id->bus == DRM_BUS_PCI && dev->pci == &i915->pci);
    CHECK("identity: GEM, mode setting, render nodes and sync objects",
          dev->features == (DRIVER_GEM | DRIVER_MODESET | DRIVER_ATOMIC | DRIVER_RENDER |
                            DRIVER_SYNCOBJ | DRIVER_SYNCOBJ_TIMELINE));
    CHECK("identity: the record found from the device", to_i915(dev) == i915);
}

/** Raise a message on `vector`, as the processor would call the handler. */
static void raise_msi(uint32_t vector)
{
    isr_params_t params;

    k_memset(&params, 0, sizeof(params));
    params.vector = vector;
    handler(&params);
}

/** Check the handler and the unplug hook of `dev`, bound to `m`, and drop
 * the device. */
static void check_unplug(struct i915_model *m, struct drm_device *dev)
{
    struct drm_i915_private *i915 = to_i915(dev);

    m->nlog = 0;
    m->log_lost = 0;
    i915->irqs = 0;
    raise_msi(48);
    raise_msi(49);
    CHECK("handler: a message on the device's vector is counted, another not", i915->irqs == 1);
    i915_unplug(dev);
    CHECK("unplug: the message off and its vector back",
          msi_disables == 1 && vectors_freed == 1 && i915->msi_vector == -1 &&
              (m->config[LENOVO_GPU_CAP_MSI + 2] & 1u) == 0);
    CHECK("unplug: every source masked again",
          i915_model_count(m, I915_MODEL_WRITE, DEIMR) == 1 && i915_model_peek(m, DEIER) == 0);
    i915->irqs = 0;
    raise_msi(48);
    CHECK("unplug: the handler forgets the device", i915->irqs == 0);
    drm_dev_put(dev);
    CHECK("release: the record freed", released == 1);
    CHECK("release: the GTT's scratch page given back", frames == 1 && frames_freed == 1);
    last_registered = 0;
}

/** Bind the driver to the model, the firmware having left two sources
 * enabled, and check what the bind did. */
static void row_bind_model(void)
{
    struct i915_model m;
    struct drm_device *dev;

    fresh(&m);
    i915_model_poke(&m, DEIER, DE_MASTER_IRQ_CONTROL | DE_PIPE_VBLANK_IVB(0));
    i915_model_poke(&m, GTIER, GT_RENDER_USER_INTERRUPT);
    fw_maps = 0;
    CHECK("bind: the model binds", i915_driver_bind(&m.pci, &m.hw) == 0);
    CHECK("bind: nothing mapped", maps == 0);
    CHECK("bind: the OpRegion at ASLS, then the boot display's ROM, sought", fw_maps == 2);
    dev = last_registered;
    CHECK("bind: one device registered", registered == 1 && dev);
    if (dev) {
        CHECK("bind: through the model",
              to_i915(dev)->hw.ops == m.hw.ops && to_i915(dev)->hw.priv == &m);
        check_masks(&m);
        check_config(&m, to_i915(dev));
        CHECK("bind: the base up: the well held, stolen memory found, the GTT's 2 GiB with the "
              "firmware's framebuffer kept",
              to_i915(dev)->power.count == 1 && to_i915(dev)->stolen.size == 0x4000000u &&
                  to_i915(dev)->ggtt.total == 0x80000000u && to_i915(dev)->ggtt.nkept == 1 &&
                  to_i915(dev)->uncore.wake_count == 0);
        CHECK("bind: without a VBT, Linux's defaults for each port",
              to_i915(dev)->opregion.base == NULL && !to_i915(dev)->vbt.present &&
                  to_i915(dev)->vbt.nencoders == INTEL_NUM_PORTS);
        check_identity(dev, to_i915(dev));
        check_unplug(&m, dev);
    }
    done(&m);
}

/** The functions, BARs, vendors and ids the driver does not bind. */
static void row_refusals(void)
{
    struct i915_model m;
    pci_device_t pdev;

    fresh(&m);
    cmdline_i915 = 1;
    pdev = m.pci;
    pdev.function = 1;
    CHECK("refuse: function 1", i915_pci_probe(&pdev) == -ENODEV && maps == 0);
    pdev = m.pci;
    pdev.bar[0] = 0;
    pdev.bar[1] = 0;
    CHECK("refuse: an unset BAR 0", i915_pci_probe(&pdev) == -ENXIO && maps == 0);
    pdev = m.pci;
    pdev.vendor_id = 0x1234;
    CHECK("refuse: another vendor", i915_driver_bind(&pdev, &m.hw) == -ENODEV);
    pdev = m.pci;
    pdev.device_id = 0x0A06;
    CHECK("refuse: a GT1 chip", i915_driver_bind(&pdev, &m.hw) == -ENODEV);
    CHECK("refuse: nothing touched",
          m.nlog == 0 && registered == 0 && i915_model_config(&m, PCI_COMMAND, 2) == 0x0407u);
    done(&m);
}

/** A machine without message vectors, and a message that will not
 * enable. */
static void row_no_messages(void)
{
    struct i915_model m;

    fresh(&m);
    msi_ready = 0;
    CHECK("no messages: binds", i915_driver_bind(&m.pci, &m.hw) == 0);
    CHECK("no messages: no vector taken", vectors == 0 && msi_enables == 0 && last_registered &&
                                              to_i915(last_registered)->msi_vector == -1);
    done(&m);

    fresh(&m);
    msi_refuse = 1;
    CHECK("refused message: binds", i915_driver_bind(&m.pci, &m.hw) == 0);
    CHECK("refused message: its vector given back", vectors == 1 && vectors_freed == 1 &&
                                                        last_registered &&
                                                        to_i915(last_registered)->msi_vector == -1);
    done(&m);
}

/** A registration that fails leaves the device as found. */
static void row_register_fails(void)
{
    struct i915_model m;

    fresh(&m);
    m.config[PCI_COMMAND] = 0x01; /* I/O only */
    m.config[PCI_COMMAND + 1] = 0x00;
    register_error = -ENOSPC;
    CHECK("register fails: the error returned", i915_driver_bind(&m.pci, &m.hw) == -ENOSPC);
    CHECK("register fails: the command word as found",
          i915_model_config(&m, PCI_COMMAND, 2) == 0x0001u);
    CHECK("register fails: the message off and its vector back",
          msi_disables == 1 && vectors_freed == 1);
    CHECK("register fails: the record released", released == 1);
    CHECK("register fails: the GTT's scratch page given back", frames == 1 && frames_freed == 1);
    CHECK("register fails: every source masked",
          i915_model_peek(&m, DEIMR) == 0xffffffffu && i915_model_peek(&m, SDEIER) == 0);
    done(&m);
}

/* What the wait rows' hook does: set `reg`'s `bit` once the clock reaches
 * `when`. */
static uint32_t hook_reg, hook_bit;
static uint64_t hook_when;

/** The model's delay hook: set the bit once the clock reaches `hook_when`. */
static void set_later(struct i915_model *m)
{
    if (m->now_us >= hook_when) {
        i915_model_poke(m, hook_reg, i915_model_peek(m, hook_reg) | hook_bit);
    }
}

/** The wait-until condition: the hook's bit of the register at `arg`. */
static int bit_is_set(struct i915_hw *hw, void *arg)
{
    return (i915_read(hw, *(uint32_t *)arg) & hook_bit) != 0;
}

/* A counter whose low half carries between the reads of the high half. */
static uint32_t carry_reads;

/** The model's read hook: a timestamp whose low half carries between the
 * first and second reads of its high half. */
static uint32_t carry(struct i915_model *m, uint32_t reg, uint32_t value)
{
    (void)m;
    if (reg == 0x2358 + 4) {
        carry_reads++;
        return carry_reads == 1 ? 0x7u : 0x8u;
    }
    if (reg == 0x2358) {
        return carry_reads == 1 ? 0x00000002u : 0xfffffff0u;
    }
    return value;
}

/** The boundary's accesses past its size, rmw, and its bounded waits. */
static void row_boundary(void)
{
    struct i915_model m;
    uint32_t reg = 0x130044, before;
    uint64_t v;

    fresh(&m);
    CHECK("boundary: past the size reads all ones",
          i915_read(&m.hw, I915_MODEL_BAR0) == 0xffffffffu && m.nlog == 0);
    CHECK("boundary: an unaligned offset reads all ones", i915_read(&m.hw, 0x2002) == 0xffffffffu);
    i915_write(&m.hw, I915_MODEL_BAR0 + 4, 1);
    CHECK("boundary: a write past the size is dropped", m.nlog == 0);
    i915_write(&m.hw, 0x44004, 0x12345678u);
    CHECK("boundary: a write reaches the register", i915_model_peek(&m, 0x44004) == 0x12345678u);
    CHECK("boundary: rmw returns the old value",
          i915_rmw(&m.hw, 0x44004, 0xff, 0x1) == 0x12345678u &&
              i915_model_peek(&m, 0x44004) == 0x12345601u);

    i915_model_poke(&m, reg, 1);
    before = m.nlog;
    CHECK("wait: a bit already set is no wait", i915_wait_for(&m.hw, "ack", reg, 1, 1, 500) == 0 &&
                                                    m.now_us == 0 && m.nlog == before + 1);

    i915_model_poke(&m, reg, 0);
    m.now_us = 0;
    before = m.reads;
    CHECK("wait: a bit never set times out",
          i915_wait_for(&m.hw, "forcewake ack", reg, 1, 1, 5000) == -ETIMEDOUT);
    CHECK("wait: at its bound exactly", m.now_us == 5000);
    CHECK("wait: names the wait", m.hw.timeouts == 1 && m.hw.last_timeout &&
                                      strcmp(m.hw.last_timeout, "forcewake ack") == 0);
    CHECK("wait: few reads, the waits doubling to 1 ms", m.reads - before <= 16);

    m.now_us = 0;
    i915_model_poke(&m, reg, 0);
    hook_reg = reg;
    hook_bit = 1;
    hook_when = 300;
    m.on_delay = set_later;
    CHECK("wait: a bit set at 300 us is seen", i915_wait_for(&m.hw, "ack", reg, 1, 1, 5000) == 0);
    CHECK("wait: soon after 300 us", m.now_us >= 300 && m.now_us < 600);

    m.now_us = 0;
    i915_model_poke(&m, reg, 0);
    hook_when = 2000;
    CHECK("wait until: a condition met at 2 ms",
          i915_wait_until(&m.hw, "fifo", bit_is_set, &reg, 5000) == 0 && m.now_us >= 2000);
    m.now_us = 0;
    i915_model_poke(&m, reg, 0);
    hook_when = 9000;
    CHECK("wait until: a condition never met times out",
          i915_wait_until(&m.hw, "fifo", bit_is_set, &reg, 5000) == -ETIMEDOUT && m.now_us == 5000);
    CHECK("wait until: names the wait",
          m.hw.last_timeout && strcmp(m.hw.last_timeout, "fifo") == 0);
    m.on_delay = 0;

    m.on_read = carry;
    carry_reads = 0;
    v = i915_read64_2x32(&m.hw, 0x2358, 0x2358 + 4);
    CHECK("read64: a carry between the halves is read again", v == 0x8fffffff0ull);
    m.on_read = 0;
    {
        uint64_t then = m.now_us;
        i915_delay_us(&m.hw, 7);
        CHECK("delay: the clock moves by the delay", m.now_us == then + 7);
    }
    done(&m);
}

/** The model itself: the Lenovo's identity and configuration space. */
static void row_model(void)
{
    struct i915_model m;

    fresh(&m);
    CHECK("model: the Lenovo's ids", m.pci.vendor_id == 0x8086 && m.pci.device_id == 0x0a16 &&
                                         m.pci.revision == 0x0b && m.pci.class_code == 3);
    CHECK("model: BAR 0 and BAR 2 decode as on the Lenovo",
          pci_bar_address64(&m.pci, 0) == LENOVO_GPU_BAR0 &&
              pci_bar_address64(&m.pci, 2) == LENOVO_GPU_BAR2);
    CHECK("model: the MSI capability first",
          i915_model_config(&m, PCI_CAP_PTR, 1) == LENOVO_GPU_CAP_MSI &&
              i915_model_config(&m, LENOVO_GPU_CAP_MSI, 1) == PCI_CAP_ID_MSI);
    CHECK("model: the blocks' table ends", i915_model_block_at(i915_model_nblocks()) == 0);
    CHECK("model: the OpRegion's address", i915_model_config(&m, 0xfc, 4) == LENOVO_GPU_ASLS);
    done(&m);
}

/** Run every row; 0 when all pass. */
int main(void)
{
    row_model();
    row_ids();
    row_probe_gated();
    row_probe_mmio();
    row_bind_model();
    row_refusals();
    row_no_messages();
    row_register_fails();
    row_boundary();
    free(map_buffer);
    free(gsm_buffer);
    if (fails) {
        printf("i915-pci-test: %d failed\n", fails);
        return 1;
    }
    printf("i915-pci-test: ok\n");
    return 0;
}
