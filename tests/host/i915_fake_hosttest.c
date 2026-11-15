/**
 * i915_fake_hosttest.c: the fake Intel GPU, rach2/sys/drm/i915/i915_fake.c,
 * which stands on QEMU's edu device for tests in QEMU, with the driver
 * bound to it through its real probe and bind.
 *
 * host-deps: tests/host/compat/i915_time_host.c
 * host-deps: rach2/sys/drm/i915/i915_pci.c rach2/sys/drm/i915/i915_hw.c
 * host-deps: rach2/sys/drm/i915/i915_uncore.c rach2/sys/drm/i915/i915_power.c
 * host-deps: rach2/sys/drm/i915/i915_stolen.c rach2/sys/drm/i915/i915_gtt.c
 * host-deps: rach2/sys/drm/i915/i915_irq.c rach2/sys/drm/i915/i915_ring.c
 * host-deps: rach2/sys/drm/i915/i915_ppgtt.c
 * host-deps: rach2/sys/drm/i915/i915_firstlight.c rach2/sys/drm/i915/i915_rc6.c
 * host-deps: rach2/sys/drm/i915/i915_rps.c rach2/sys/tick.c
 * host-deps: rach2/sys/drm/i915/intel_bios.c
 * host-deps: rach2/sys/drm/i915/intel_opregion.c
 * host-deps: rach2/sys/lib/kstring.c rach2/sys/lib/kctypes.c
 * host-deps: rach2/sys/mm/frame.c tests/host/compat/spinlock_host.c
 * host-deps: rach2/sys/drm/i915/i915_aperture.c rach2/sys/drm/i915/i915_fence.c
 * host-deps: tests/host/compat/i915_gem_lock_host.c
 * host-deps: tests/host/compat/mutex_none.c
 * host-deps: tests/host/compat/i915_rpm_host.c
 *
 * i915_fake.c is included whole, which reaches its record and its
 * debugfs file's operations. The bus holds one edu function, 00:04.0,
 * whose command word and BAR 0 the PCI calls read; the DRM core is a
 * record of the device registered; debugfs is a record of the file made.
 * The heap's pages have physical addresses of their own, which
 * `kmap_temp` maps back, so the fake's rings reach the driver's rings and
 * status pages through the GTT as on the machine. The processor takes the
 * device's message as soon as its interrupts are on: at once when they
 * are, and otherwise when the last `kirq_restore` turns them on again,
 * with its interrupts off while the handler runs. A row that keeps edu's
 * interrupt register in place of that sees the writes the message makes.
 *
 * The rows: the probe binds the driver to edu with the Haswell identity
 * reported, the GTT and aperture the fake's configuration space gives, no
 * stolen memory, both rings started, the message on edu, the debugfs file
 * made, and no bounded wait timed out; the probe with no edu; the
 * registers that do more than hold a value; first light run from the
 * file, its interrupt taken through the handler; the message as a write
 * to edu's interrupt register; each fault, `submit`, `bind`, `irq` and
 * `hang`, armed from the file, its injection counted and logged and
 * first light failing by name, and a hung ring reset through GDRST;
 * faults armed for always and cleared, and the commands the file
 * refuses; the commands later parts give the rings, PIPE_CONTROL and
 * MI_FLUSH_DW's post-sync writes, a batch completed at once, commands
 * passed over by their length, and a command of no known type that
 * hangs the ring; the file read and written through its operations; and
 * the unplug, which removes the file, and the release, which frees the
 * fake.
 *
 * Returns 0 when every row passes, 1 otherwise.
 */

#include <stdarg.h>

#include "drm/i915/i915_fake.c"
#include "arch/x86/portio.h"
#include "fs/fd.h"
#include "fs/sysfs.h"
#include "iommu.h"
#include "lib/kirq.h"
#include "proc/percpu.h"
#include "rtc.h"

/* The boot processor's tick count, which forcewake's release timer reads. */
volatile uint64_t kernel_ticks;

extern int printf(const char *format, ...);
extern int snprintf(char *s, unsigned long n, const char *format, ...);
extern int vsnprintf(char *s, unsigned long n, const char *format, va_list ap);
extern int posix_memalign(void **p, unsigned long align, unsigned long size);
extern void *calloc(unsigned long count, unsigned long size);
extern void *malloc(unsigned long size);
extern void free(void *p);
extern char *strstr(const char *s, const char *t);
extern int strcmp(const char *a, const char *b);

static int fails;

#define CHECK(what, cond)                               \
    do {                                                \
        if (!(cond)) {                                  \
            printf("i915-fake-test: fail: %s\n", what); \
            fails++;                                    \
        }                                               \
    } while (0)

/* ---- the kernel around the driver ---- */

/** The fake backend has no VGA I/O ports and never reads one. */
uint8_t inb(uint16_t port)
{
    (void)port;
    return 0;
}

/** The fake backend has no VGA I/O ports and never writes one. */
void outb(uint16_t port, uint8_t value)
{
    (void)port;
    (void)value;
}

static char log_text[32768];

/** The boot log, kept whole for the rows that read it. */
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

static int cmdline_fake;

int kcmdline_has(const char *flag)
{
    return cmdline_fake && strcmp(flag, "rach2.i915fake") == 0;
}

/** No `nomodeset` on the boot line. */
int kcmdline_setup(const char *str)
{
    (void)str;
    return 0;
}

/** No module parameter on the boot line. */
int kcmdline_param_int(const char *key, int dflt)
{
    (void)key;
    return dflt;
}

static uint32_t udelays;

void k_udelay(uint32_t usec)
{
    udelays += usec;
}

/* The processor: its interrupts off while `irq_depth` is above 0, and on
 * otherwise once a row sets `irq_on`; `pending` messages wait for them. */
static int irq_depth, irq_on, pending, taken;
static struct drm_i915_private *bound;

/** Take the messages that wait, while the processor's interrupts are on,
 * each with them off for the handler, as the processor does. */
static void take_messages(void)
{
    while (pending > 0 && irq_on && irq_depth == 0 && bound) {
        pending--;
        taken++;
        irq_depth++;
        i915_irq_handle(bound);
        irq_depth--;
    }
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
    take_messages();
}

/** The fake's message, as the row puts it in place of edu's register. */
static void test_raise(struct i915_fake *f)
{
    (void)f;
    pending++;
    take_messages();
}

/* The heap's pages: each a host page with a physical address of its own,
 * two pages apart from the last. */
#define TEST_PAGES 256
static struct {
    uint8_t *host;
    uint64_t phys;
} pages[TEST_PAGES];
static int npages, pages_live;
static uint64_t next_phys = 0x50000000ull;

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

static int heap_live;

void *kmalloc(size_t size)
{
    void *p = malloc(size ? size : 1);

    heap_live += p != 0;
    return p;
}

void *kzalloc(size_t size)
{
    void *p = calloc(1, size ? size : 1);

    heap_live += p != 0;
    return p;
}

void kfree(void *p)
{
    heap_live -= p != 0;
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

/* The GTT's scratch page. */
#define TEST_SCRATCH 0x7f000000u
static uint32_t scratch_page[1024];
static int kmaps;

/** The host page at `phys`, as the direct map gives it. */
void *kmap_temp(uintptr_t phys)
{
    int i;

    kmaps++;
    if ((phys & ~0xfffu) == TEST_SCRATCH) {
        return (uint8_t *)scratch_page + (phys & 0xfffu);
    }
    for (i = 0; i < npages; i++) {
        if (pages[i].host && pages[i].phys == (phys & ~(uintptr_t)0xfffu)) {
            return pages[i].host + (phys & 0xfffu);
        }
    }
    return 0;
}

void kunmap_temp(void *va)
{
    (void)va;
}

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

int paging_frame_is_device(uintptr_t phys)
{
    (void)phys;
    return 1;
}

void arch_dma_fence(void)
{}

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
    (void)b;
    (void)devfn;
    (void)out;
    (void)max;
    return 0;
}

/* The MMIO backend, which no row uses. */
int pat_write_combining(void)
{
    return 1;
}

uintptr_t map_mmio_typed(uintptr_t phys_addr, size_t size, pte_t cache)
{
    (void)phys_addr;
    (void)size;
    (void)cache;
    return 0;
}

/* edu's BAR 0, as `mmio_map` maps it: a buffer of the test's. */
#define TEST_EDU_BAR0 0xfea00000u
static uint32_t *edu_regs;
static int edu_maps, edu_unmaps;

uintptr_t mmio_map(uint64_t phys, size_t size, pte_t cache)
{
    (void)cache;
    if (phys != TEST_EDU_BAR0 || edu_regs) {
        return 0;
    }
    edu_maps++;
    edu_regs = (uint32_t *)calloc(size / 4, sizeof(uint32_t));
    return (uintptr_t)edu_regs;
}

void mmio_unmap(uintptr_t va, size_t size)
{
    (void)size;
    if (va && va == (uintptr_t)edu_regs) {
        edu_unmaps++;
        free(edu_regs);
        edu_regs = 0;
    }
}

/* The bus: one edu function at 00:04.0, or none. */
static int edu_present;
static uint8_t edu_config[256];
static pci_device_t edu;

/** Whether (bus, device, function) is edu's. */
static int is_edu(uint8_t b, uint8_t d, uint8_t f)
{
    return edu_present && b == 0 && d == 4 && f == 0;
}

uint32_t pci_read_config32(uint8_t b, uint8_t d, uint8_t f, uint8_t offset)
{
    if (!is_edu(b, d, f)) {
        return 0xffffffffu;
    }
    return (uint32_t)edu_config[offset] | (uint32_t)edu_config[offset + 1] << 8 |
           (uint32_t)edu_config[offset + 2] << 16 | (uint32_t)edu_config[offset + 3] << 24;
}

uint16_t pci_read_config16(uint8_t b, uint8_t d, uint8_t f, uint8_t offset)
{
    return is_edu(b, d, f) ? (uint16_t)(edu_config[offset] | edu_config[offset + 1] << 8) : 0xffffu;
}

void pci_write_config16(uint8_t b, uint8_t d, uint8_t f, uint8_t offset, uint16_t value)
{
    if (is_edu(b, d, f)) {
        edu_config[offset] = (uint8_t)value;
        edu_config[offset + 1] = (uint8_t)(value >> 8);
    }
}

void pci_enable_bus_master(pci_device_t *dev)
{
    pci_write_config16(
        dev->bus, dev->device, dev->function, PCI_COMMAND,
        (uint16_t)(pci_read_config16(dev->bus, dev->device, dev->function, PCI_COMMAND) |
                   PCI_CMD_BUS_MASTER));
}

void pci_enable_memory_space(pci_device_t *dev)
{
    pci_write_config16(
        dev->bus, dev->device, dev->function, PCI_COMMAND,
        (uint16_t)(pci_read_config16(dev->bus, dev->device, dev->function, PCI_COMMAND) |
                   PCI_CMD_MEMORY_SPACE));
}

/* No row here fails a bind; i915_pci_hosttest.c checks the claim a failed
 * bind gives back. */
const char *pci_owner(uint8_t b, uint8_t devfn)
{
    (void)b;
    (void)devfn;
    return 0;
}

void pci_unclaim(uint8_t b, uint8_t devfn, const char *who)
{
    (void)b;
    (void)devfn;
    (void)who;
}

bool pci_find_nth_by_id(uint16_t vendor_id, uint16_t device_id, int nth, pci_device_t *out)
{
    if (edu_present && nth == 0 && vendor_id == edu.vendor_id && device_id == edu.device_id) {
        *out = edu;
        return true;
    }
    return false;
}

uint64_t pci_bar_address64(const pci_device_t *dev, int i)
{
    return i == 0 && is_edu(dev->bus, dev->device, dev->function) ? TEST_EDU_BAR0 : 0;
}

static int msi_vector_on = -1;

int pci_msi_enable(const pci_device_t *dev, uint8_t vector)
{
    if (!is_edu(dev->bus, dev->device, dev->function)) {
        return -1;
    }
    msi_vector_on = vector;
    return 0;
}

void pci_msi_disable(const pci_device_t *dev)
{
    (void)dev;
    msi_vector_on = -1;
}

int isr_msi_ready(void)
{
    return 1;
}

int isr_alloc_vector(void)
{
    return 48;
}

void isr_free_vector(int vector)
{
    (void)vector;
}

void register_isr_handler(uint8_t vector, isr_handler_t h, const char *name)
{
    (void)vector;
    (void)h;
    (void)name;
}

/** Take the handler's flag, which a test of one thread does not need. */
void isr_register_flags(uint8_t vector, isr_handler_t handler, unsigned int flags)
{
    (void)vector;
    (void)handler;
    (void)flags;
}

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

/* The DRM core: the device registered, with its reference count. */
static struct drm_device *registered;
static int released;

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
    registered = dev;
    dev->registered = 1;
    dev->index = 1;
    k_strncpy(dev->unique, "pci:0000:00:04.0", sizeof(dev->unique) - 1);
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

void intel_dp_aux_dev_register(struct drm_i915_private *i915)
{
    (void)i915;
}

void intel_dp_aux_dev_unregister(struct drm_i915_private *i915)
{
    (void)i915;
}

void intel_i2c_register(struct drm_i915_private *i915)
{
    (void)i915;
}

void intel_i2c_unregister(struct drm_i915_private *i915)
{
    (void)i915;
}

/* The release and the bring-up through /sys, i915_handoff.c, which
 * i915_handoff_hosttest.c tests; no row here unbinds the driver. */
int32_t i915_handoff_link(struct drm_i915_private *i915)
{
    (void)i915;
    return 0;
}

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

void drm_dev_get(struct drm_device *dev)
{
    dev->refs++;
}

void drm_dev_put(struct drm_device *dev)
{
    if (--dev->refs == 0 && dev->driver->release) {
        released++;
        dev->driver->release(dev);
    }
}

/** The path of a file in the device's debugfs directory, by its PCI
 * function as on the machine. */
void drm_debugfs_path(const struct drm_device *dev, const char *name, char *path, uint32_t cap)
{
    (void)dev;
    snprintf(path, cap, "dri/0000:00:04.0/%s", name);
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

/* debugfs: the one file made, while it is there. */
static char dfs_path[64];
static const struct file_ops *dfs_ops;
static debugfs_open_fn dfs_open;
static void *dfs_data;
static uint32_t dfs_mode;

int32_t debugfs_create_file(const char *path, uint32_t mode, const struct file_ops *ops,
                            debugfs_open_fn open, void *data)
{
    if (dfs_ops) {
        return -EEXIST;
    }
    k_strncpy(dfs_path, path, sizeof(dfs_path) - 1);
    dfs_mode = mode;
    dfs_ops = ops;
    dfs_open = open;
    dfs_data = data;
    return 0;
}

void debugfs_remove(const char *path)
{
    if (dfs_ops && strcmp(path, dfs_path) == 0) {
        dfs_ops = 0;
        dfs_path[0] = 0;
    }
}

int copy_from_user(void *dst, const void __user *src, size_t n)
{
    k_memcpy(dst, (const void *)src, n);
    return 0;
}

int copy_to_user(void __user *dst, const void *src, size_t n)
{
    k_memcpy((void *)dst, src, n);
    return 0;
}

/* ---- rows ---- */

/** Put edu on the bus, clear the kernel's records, and bind the driver to
 * the fake through the probe, with the processor's interrupts off as at
 * boot; the fake, or NULL. Its message is the test's unless `edu_raise`. */
static struct i915_fake *fresh(int edu_raise)
{
    struct i915_fake *f;

    log_text[0] = 0;
    k_memset(edu_config, 0, sizeof(edu_config));
    edu_config[0] = 0x34;
    edu_config[1] = 0x12;
    edu_config[2] = 0xe8;
    edu_config[3] = 0x11;
    edu_config[8] = 0x10;
    k_memset(&edu, 0, sizeof(edu));
    edu.bus = 0;
    edu.device = 4;
    edu.function = 0;
    edu.vendor_id = FAKE_EDU_VENDOR;
    edu.device_id = FAKE_EDU_DEVICE;
    edu.revision = 0x10;
    edu.bar[0] = TEST_EDU_BAR0;
    edu_present = 1;
    cmdline_fake = 1;
    irq_on = 0;
    irq_depth = 0;
    pending = taken = 0;
    registered = 0;
    released = 0;
    bound = 0;
    udelays = 0;
    CHECK("probe: binds one", i915_probe() == 1 && registered);
    if (!registered) {
        return 0;
    }
    bound = to_i915(registered);
    f = (struct i915_fake *)bound->hw.priv;
    if (!edu_raise) {
        f->raise = test_raise;
    }
    irq_on = 1;
    return f;
}

/** Unplug the device and drop the last reference, as the DRM core does. */
static void done(void)
{
    if (registered) {
        registered->driver->unplug(registered);
        drm_dev_put(registered);
    }
    registered = 0;
    bound = 0;
    edu_present = 0;
}

/** Run a command of the debugfs file, as a write of it does. */
static int32_t cmd(struct i915_fake *f, const char *s)
{
    return fake_command(f, s, (uint32_t)k_strlen(s));
}

/** The identity the probe gave the device: the fake's, reported for
 * edu's function. */
static void check_identity(void)
{
    const struct sysfs_pci_ident *id;

    CHECK("probe: logged", logged("fake: 00:04.0: edu stands in for 8086:0a16") &&
                               logged("found: 00:04.0: haswell/ult (device id 0a16)") &&
                               logged("revision 0b"));
    CHECK("probe: the fake is the backend",
          bound->hw.ops == &fake_ops && bound->id && bound->id->device_id == 0x0A16);
    CHECK("probe: the function is edu's", registered->pci == &bound->pci &&
                                              bound->pci.vendor_id == FAKE_EDU_VENDOR &&
                                              bound->pci.device == 4);
    id = registered->pci_ident;
    CHECK("probe: /sys reports the HD 4400",
          id && id->vendor == 0x8086 && id->device == 0x0a16 && id->subsystem_vendor == 0x17aa &&
              id->subsystem_device == 0x380c && id->class == 0x030000 && id->revision == 0x0b &&
              id->boot_vga == 0);
}

/** The base and rings the bind brought up on the fake `f`, with no wait
 * reaching its bound. */
static void check_base(struct i915_fake *f)
{
    CHECK("probe: no stolen memory, a 2 GiB GTT and a 256 MiB aperture",
          bound->stolen.size == 0 && bound->ggtt.total == 0x80000000u &&
              bound->ggtt.mappable_end == 0x10000000u && f->gtt_entries == 524288u &&
              bound->hw.gsm == f->gtt);
    CHECK("probe: every entry on the scratch page",
          f->gtt[0] == bound->ggtt.scratch_pte && f->gtt[1000] == bound->ggtt.scratch_pte);
    CHECK("probe: the four rings started",
          bound->gt.engine[I915_RCS0].running && bound->gt.engine[I915_BCS0].running &&
              bound->gt.engine[I915_VCS0].running && bound->gt.engine[I915_VECS0].running &&
              (R(f, RING_CTL(RENDER_RING_BASE)) & RING_VALID) &&
              (R(f, RING_CTL(BLT_RING_BASE)) & RING_VALID) &&
              (R(f, RING_CTL(GEN6_BSD_RING_BASE)) & RING_VALID) &&
              (R(f, RING_CTL(VEBOX_RING_BASE)) & RING_VALID));
    CHECK("probe: no wait timed out", bound->hw.timeouts == 0 && bound->uncore.fifo_timeouts == 0 &&
                                          bound->uncore.thread_timeouts == 0);
}

/** The message, the debugfs file and the interrupt masks after the bind
 * to the fake `f`. */
static void check_after_bind(struct i915_fake *f)
{
    CHECK("probe: the message on edu", bound->msi_vector == 48 && msi_vector_on == 48);
    CHECK("probe: edu's command word decodes and masters",
          (edu_config[PCI_COMMAND] & (PCI_CMD_MEMORY_SPACE | PCI_CMD_BUS_MASTER)) ==
              (PCI_CMD_MEMORY_SPACE | PCI_CMD_BUS_MASTER));
    CHECK("probe: the file made", dfs_ops == &fake_file_ops &&
                                      strcmp(dfs_path, "dri/0000:00:04.0/i915_fake") == 0 &&
                                      dfs_mode == 0600u && dfs_data == f && f->i915 == bound);
    CHECK("probe: edu's registers mapped", edu_maps == 1 && f->doorbell == (uintptr_t)edu_regs);
    CHECK("probe: every source masked but the user interrupts and the master enable",
          R(f, GTIMR) == 0xffffffffu && R(f, DEIMR) == 0xffffffffu &&
              R(f, GEN6_PMIMR) == 0xffffffffu &&
              R(f, GTIER) ==
                  (GT_RENDER_USER_INTERRUPT | GT_BLT_USER_INTERRUPT | GT_BSD_USER_INTERRUPT) &&
              (R(f, GEN6_PMIER) & PM_VEBOX_USER_INTERRUPT) &&
              R(f, DEIER) == DE_MASTER_IRQ_CONTROL && f->messages == 0);
}

/** The probe: edu bound as the HD 4400, its identity reported, the base
 * and rings up without a single wait reaching its bound. */
static void row_probe(void)
{
    struct i915_fake *f = fresh(0);

    if (!f) {
        return;
    }
    check_identity();
    check_base(f);
    check_after_bind(f);
    done();
    CHECK("unplug: the file gone and edu unmapped",
          dfs_ops == 0 && edu_unmaps == 1 && edu_maps == 1);
    CHECK("release: the record and the fake freed",
          released == 1 && heap_live == 0 && pages_live == 0);
    edu_maps = edu_unmaps = 0;
}

/** The probe without edu, and without the option. */
static void row_no_edu(void)
{
    log_text[0] = 0;
    edu_present = 0;
    cmdline_fake = 1;
    registered = 0;
    CHECK("no edu: nothing bound", i915_probe() == -ENODEV && registered == 0);
    CHECK("no edu: logged", logged("fake: no edu device to stand on"));
    edu_present = 1;
    cmdline_fake = 0;
    edu.vendor_id = FAKE_EDU_VENDOR;
    edu.device_id = FAKE_EDU_DEVICE;
    CHECK("no option: nothing bound", i915_probe() == -ENODEV && registered == 0);
    edu_present = 0;
    CHECK("no edu: nothing left", heap_live == 0);
}

/** The registers that do more than hold what was written. */
static void row_registers(void)
{
    struct i915_fake *f = fresh(0);
    struct i915_hw *hw;

    if (!f) {
        return;
    }
    hw = &bound->hw;
    i915_write(hw, FORCEWAKE_MT, I915_MASKED_ENABLE(FORCEWAKE_KERNEL));
    CHECK("forcewake: acknowledged at once", i915_read(hw, FORCEWAKE_ACK_HSW) == FORCEWAKE_KERNEL);
    i915_write(hw, FORCEWAKE_MT, I915_MASKED_DISABLE(FORCEWAKE_KERNEL));
    CHECK("forcewake: released", i915_read(hw, FORCEWAKE_ACK_HSW) == 0);
    CHECK("gt fifo: always room", (i915_read(hw, GTFIFOCTL) & GT_FIFO_FREE_ENTRIES_MASK) == 64u);
    CHECK("power well: on while requested",
          (i915_read(hw, HSW_PWR_WELL_CTL2) & HSW_PWR_WELL_CTL_STATE(HSW_PW_CTL_IDX_GLOBAL)) != 0);
    i915_write(hw, HSW_PWR_WELL_CTL2, 0);
    CHECK("power well: off with no request",
          (i915_read(hw, HSW_PWR_WELL_CTL1) & HSW_PWR_WELL_CTL_STATE(HSW_PW_CTL_IDX_GLOBAL)) == 0);
    i915_write(hw, HSW_PWR_WELL_CTL2, HSW_PWR_WELL_CTL_REQ(HSW_PW_CTL_IDX_GLOBAL) | 0x1234u);
    CHECK("power well: only the request kept",
          i915_read(hw, HSW_PWR_WELL_CTL2) == (HSW_PWR_WELL_CTL_REQ(HSW_PW_CTL_IDX_GLOBAL) |
                                               HSW_PWR_WELL_CTL_STATE(HSW_PW_CTL_IDX_GLOBAL)));
    R(f, GEN7_ERR_INT) = 0x5u;
    i915_write(hw, GEN7_ERR_INT, 0x1u);
    CHECK("error register: the bits written clear", R(f, GEN7_ERR_INT) == 0x4u);
    i915_write(hw, GTISR, 0xffu);
    CHECK("status register: takes nothing", R(f, GTISR) == 0);
    i915_write(hw, RING_INSTPM(RENDER_RING_BASE),
               I915_MASKED_ENABLE(INSTPM_SYNC_FLUSH | INSTPM_TLB_INVALIDATE));
    CHECK("instpm: the flush done at once", (i915_read(hw, RING_INSTPM(RENDER_RING_BASE)) &
                                             (INSTPM_SYNC_FLUSH | INSTPM_TLB_INVALIDATE)) == 0);
    CHECK("mi_mode: running, not idle",
          (i915_read(hw, RING_MI_MODE(RENDER_RING_BASE)) & (STOP_RING | MODE_IDLE)) == 0);
    i915_write(hw, RING_MI_MODE(RENDER_RING_BASE), I915_MASKED_ENABLE(STOP_RING));
    CHECK("mi_mode: stopped and empty is idle",
          (i915_read(hw, RING_MI_MODE(RENDER_RING_BASE)) & (STOP_RING | MODE_IDLE)) ==
              (STOP_RING | MODE_IDLE));
    i915_write(hw, RING_MI_MODE(RENDER_RING_BASE), I915_MASKED_DISABLE(STOP_RING));
    CHECK("config: the identity and the GMCH words",
          fake_config_read32(hw, 0) == 0x0a168086u && fake_config_read32(hw, 8) == 0x0300000bu &&
              fake_config_read32(hw, 0x2c) == 0x380c17aau &&
              fake_config_read32(hw, SNB_GMCH_CTRL) == 0x200u &&
              (fake_config_read32(hw, 0x60) >> 16) == 0x2u &&
              fake_config_read32(hw, INTEL_BSM) == 0);
    done();
}

/** First light from the file: the store and the breadcrumb done at once,
 * the user interrupt raised and taken through the handler. */
static void row_light(void)
{
    struct i915_fake *f = fresh(0);
    struct i915_engine *e;

    if (!f) {
        return;
    }
    e = &bound->gt.engine[I915_RCS0];
    CHECK("light: before it ran", f->light == 1);
    CHECK("light: runs", cmd(f, "light\n") == 0 && f->light == 0);
    CHECK("light: logged",
          logged("first light: ok: rcs0 wrote 0x1915f157") && logged("user interrupt taken"));
    CHECK("light: the breadcrumb in the status page",
          e->hws[I915_GEM_HWS_SEQNO] == e->seqno && e->seqno != 0);
    CHECK("light: the head at the tail",
          (R(f, RING_HEAD(RENDER_RING_BASE)) & HEAD_ADDR) == e->tail &&
              R(f, RING_ACTHD(RENDER_RING_BASE)) == e->ring_ggtt + e->tail);
    CHECK("light: one message, taken",
          f->messages == 1 && taken == 1 && e->irq_seen == 1 && pending == 0);
    CHECK("light: the identity register clear and the user interrupt masked again",
          R(f, GTIIR) == 0 && (R(f, GTIMR) & GT_RENDER_USER_INTERRUPT) &&
              (R(f, DEIER) & DE_MASTER_IRQ_CONTROL));
    CHECK("light: commands counted", f->ring[0].commands == 3 && f->ring[0].batches == 0);
    CHECK("light: again", cmd(f, "light") == 0 && f->light == 0 && f->messages == 2);
    done();
}

/** The message through edu: a write to its interrupt register for each
 * rise of the line, while the driver has its message on. */
static void row_edu_message(void)
{
    struct i915_fake *f = fresh(1);

    if (!f) {
        return;
    }
    CHECK("edu: the edu raise in place", f->raise == fake_raise_edu);
    CHECK("edu: light runs", cmd(f, "light") == 0 && f->light == 0);
    CHECK("edu: the raise register written", edu_regs && edu_regs[FAKE_EDU_RAISE / 4] == 1u);
    CHECK("edu: the interrupt latched, since no handler ran",
          logged("user interrupt latched") && f->messages == 1);
    edu_regs[FAKE_EDU_RAISE / 4] = 0;
    bound->msi_vector = -1;
    i915_write(&bound->hw, GTIIR, 0xffffffffu);
    CHECK("edu: no write without the message",
          cmd(f, "light") == 0 && edu_regs[FAKE_EDU_RAISE / 4] == 0);
    bound->msi_vector = 48;
    done();
}

/** The submit fault, armed from the file, injected once, counted and
 * logged, and first light refused by name. */
static void row_fault_submit(void)
{
    struct i915_fake *f = fresh(0);

    if (!f) {
        return;
    }
    CHECK("submit: armed", cmd(f, "submit") == 0 && f->armed[FAKE_SUBMIT] == 1);
    CHECK("submit: refused", cmd(f, "light") == 0 && f->light == -EIO);
    CHECK("submit: logged",
          logged("fake: injected: submit: error -5") && logged("first light: submission refused"));
    CHECK("submit: once",
          f->armed[FAKE_SUBMIT] == 0 && f->injected[FAKE_SUBMIT] == 1 && bound->hw.faults == 1);
    CHECK("submit: then light", cmd(f, "light") == 0 && f->light == 0);
    done();
}

/** The bind fault: first light without its page, and the GTT refusing
 * with ENOSPC as many times as armed. */
static void row_fault_bind(void)
{
    struct i915_fake *f = fresh(0);

    if (!f) {
        return;
    }
    CHECK("bind: armed", cmd(f, "bind") == 0);
    CHECK("bind: no page", cmd(f, "light") == 0 && f->light == -ENOMEM);
    CHECK("bind: logged", logged("fake: injected: bind: error -28") &&
                              logged("first light: no page for the value"));
    CHECK("bind: counted", f->injected[FAKE_BIND] == 1 && f->armed[FAKE_BIND] == 0);
    {
        uint64_t page = TEST_SCRATCH;
        CHECK("bind: twice", cmd(f, "bind 2") == 0 && f->armed[FAKE_BIND] == 2);
        CHECK("bind: the GTT refuses with ENOSPC twice, then binds",
              i915_ggtt_insert(bound, 0, &page, 1, I915_CACHE_NONE) == -ENOSPC &&
                  i915_ggtt_insert(bound, 0, &page, 1, I915_CACHE_NONE) == -ENOSPC &&
                  i915_ggtt_insert(bound, 0, &page, 1, I915_CACHE_NONE) == 0 &&
                  f->injected[FAKE_BIND] == 3);
    }
    done();
}

/** A lost interrupt: the breadcrumb written, no user interrupt. */
static void row_fault_irq(void)
{
    struct i915_fake *f = fresh(0);
    struct i915_engine *e;

    if (!f) {
        return;
    }
    e = &bound->gt.engine[I915_RCS0];
    CHECK("irq: armed", cmd(f, "irq") == 0);
    CHECK("irq: lost", cmd(f, "light") == 0 && f->light == -ETIME);
    CHECK("irq: logged", logged("fake: injected: irq: rcs0's user interrupt lost") &&
                             logged("first light: no user interrupt"));
    CHECK("irq: the breadcrumb written all the same", e->hws[I915_GEM_HWS_SEQNO] == e->seqno);
    CHECK("irq: then light", cmd(f, "light") == 0 && f->light == 0);
    done();
}

/** A hang: the ring stuck at the next command, until a reset of its
 * domain through GDRST. */
static void row_fault_hang(void)
{
    struct i915_fake *f = fresh(0);
    struct i915_engine *e;
    uint32_t start, head;

    if (!f) {
        return;
    }
    e = &bound->gt.engine[I915_RCS0];
    start = e->ring_ggtt;
    head = R(f, RING_HEAD(RENDER_RING_BASE)) & HEAD_ADDR;
    CHECK("hang: armed", cmd(f, "hang") == 0);
    CHECK("hang: no breadcrumb", cmd(f, "light") == 0 && f->light == -ETIME);
    CHECK("hang: logged",
          logged("fake: injected: hang: rcs0") && logged("first light: no breadcrumb"));
    CHECK("hang: the ring stuck at the store",
          f->ring[0].hung && (R(f, RING_HEAD(RENDER_RING_BASE)) & HEAD_ADDR) == head &&
              R(f, RING_ACTHD(RENDER_RING_BASE)) == start + head &&
              R(f, RING_IPEHR(RENDER_RING_BASE)) == (MI_STORE_DWORD_IMM_GEN4 | MI_USE_GGTT));
    CHECK("hang: stays hung",
          cmd(f, "light") == 0 && f->light == -ETIME && f->injected[FAKE_HANG] == 1);
    i915_write(&bound->hw, RING_MI_MODE(RENDER_RING_BASE), I915_MASKED_ENABLE(STOP_RING));
    CHECK("hang: told to stop, never idle", (i915_read(&bound->hw, RING_MI_MODE(RENDER_RING_BASE)) &
                                             (STOP_RING | MODE_IDLE)) == STOP_RING);
    i915_write(&bound->hw, RING_MI_MODE(RENDER_RING_BASE), I915_MASKED_DISABLE(STOP_RING));
    i915_write(&bound->hw, GEN6_GDRST, GEN6_GRDOM_RENDER);
    CHECK("hang: a reset of the render domain ends it",
          !f->ring[0].hung && R(f, GEN6_GDRST) == 0 && R(f, RING_CTL(RENDER_RING_BASE)) == 0 &&
              R(f, RING_HEAD(RENDER_RING_BASE)) == 0 &&
              R(f, RING_IMR(RENDER_RING_BASE)) == 0xffffffffu &&
              (R(f, RING_CTL(BLT_RING_BASE)) & RING_VALID));
    done();
}

/** Faults for always, clear, a count of 0, and the commands refused. */
static void row_fault_commands(void)
{
    struct i915_fake *f = fresh(0);
    struct fake_text t;

    if (!f) {
        return;
    }
    CHECK("always: armed", cmd(f, "submit always") == 0 && f->armed[FAKE_SUBMIT] == FAKE_ALWAYS);
    CHECK("always: refused twice", cmd(f, "light") == 0 && f->light == -EIO &&
                                       cmd(f, "light") == 0 && f->light == -EIO &&
                                       f->armed[FAKE_SUBMIT] == FAKE_ALWAYS);
    CHECK("count: armed", cmd(f, "hang 3") == 0 && f->armed[FAKE_HANG] == 3);
    CHECK("count: 0 disarms", cmd(f, "hang 0") == 0 && f->armed[FAKE_HANG] == 0);
    CHECK("count: armed again", cmd(f, "irq   12 ") == 0 && f->armed[FAKE_IRQ] == 12);
    t.f = f;
    fake_text(f, &t);
    CHECK("text: each fault, ring and the light",
          strstr(t.text, "fault submit armed always injected 2\n") &&
              strstr(t.text, "fault bind armed 0 injected 0\n") &&
              strstr(t.text, "fault irq armed 12 injected 0\n") &&
              strstr(t.text, "ring rcs0 commands 0 batches 0 hung 0\n") &&
              strstr(t.text, "ring bcs0 commands 0 batches 0 hung 0\n") &&
              strstr(t.text, "messages 0\n") && strstr(t.text, "light error -5\n"));
    CHECK("clear", cmd(f, "clear") == 0 && f->armed[FAKE_SUBMIT] == 0 && f->armed[FAKE_IRQ] == 0);
    CHECK("clear: then light", cmd(f, "light") == 0 && f->light == 0);
    CHECK("refused: an unknown fault", cmd(f, "melt") == -EINVAL);
    CHECK("refused: a count that is no number",
          cmd(f, "hang x") == -EINVAL && cmd(f, "hang 12x") == -EINVAL);
    CHECK("refused: a count too large", cmd(f, "hang 99999999999") == -EINVAL);
    CHECK("refused: words after clear and light",
          cmd(f, "clear all") == -EINVAL && cmd(f, "light now") == -EINVAL);
    CHECK("refused: nothing", cmd(f, "") == -EINVAL && cmd(f, "\n") == -EINVAL);
    CHECK("refused: a word with NULs, read no further than the word's name",
          fake_command(f, "clear\0\0\0\0", 9) == -EINVAL &&
              fake_command(f, "hang\0\0\0\0\0", 9) == -EINVAL);
    CHECK("refused: nothing armed", f->armed[FAKE_HANG] == 0);
    done();
}

/** Write `n` commands to `e`'s ring and hand them to the chip. */
static void emit(struct i915_engine *e, const uint32_t *cs, uint32_t n)
{
    uint32_t *at = i915_ring_begin(e, bound, n);
    uint32_t i;

    CHECK("emit: room", at != 0);
    if (!at) {
        return;
    }
    for (i = 0; i < n; i++) {
        at[i] = cs[i];
    }
    i915_ring_advance(e, bound);
}

/** The commands later parts give the rings. */
static void row_commands(void)
{
    struct i915_fake *f = fresh(0);
    struct i915_engine *rcs, *bcs;
    uint32_t *page, ggtt = 0, before;

    if (!f) {
        return;
    }
    rcs = &bound->gt.engine[I915_RCS0];
    bcs = &bound->gt.engine[I915_BCS0];
    page = (uint32_t *)i915_gt_pages_map(bound, 1, I915_CACHE_LLC, &ggtt);
    CHECK("commands: a page", page != 0);
    if (!page) {
        done();
        return;
    }
    {
        /* gen7_emit_breadcrumb_rcs: a PIPE_CONTROL that stores the
         * sequence number by its index, then MI_USER_INTERRUPT. */
        const uint32_t cs[] = {FAKE_PIPE_CONTROL | 2u,
                               FAKE_POST_SYNC_WRITE | FAKE_STORE_INDEX | (1u << 24) | (1u << 20),
                               I915_GEM_HWS_SEQNO_ADDR,
                               77u,
                               MI_USER_INTERRUPT,
                               MI_NOOP};
        i915_engine_irq_get(bound, rcs);
        emit(rcs, cs, 6);
        CHECK("pipe_control: stored by index", rcs->hws[I915_GEM_HWS_SEQNO] == 77u);
        CHECK("pipe_control: the interrupt taken", rcs->irq_seen == 1);
    }
    {
        /* A PIPE_CONTROL to an address with notify, a 3D command passed
         * over by its length, and a batch. */
        const uint32_t cs[] = {FAKE_PIPE_CONTROL | 2u,
                               FAKE_POST_SYNC_WRITE | FAKE_PIPE_CONTROL_NOTIFY,
                               ggtt + 8u,
                               0xabcdu,
                               0x78000000u | 1u,
                               0xdeadbeefu,
                               0xdeadbeefu,
                               MI_INSTR(FAKE_MI_BATCH_BUFFER_START, 0),
                               0x00100000u,
                               MI_NOOP};
        emit(rcs, cs, 10);
        CHECK("pipe_control: stored at the address", page[2] == 0xabcdu);
        CHECK("pipe_control: notify raises the interrupt", rcs->irq_seen == 2);
        CHECK("batch: completed at once", f->ring[0].batches == 1 && !f->ring[0].hung);
        i915_engine_irq_put(bound, rcs);
    }
    {
        /* gen6_emit_breadcrumb_xcs on the blitter: MI_FLUSH_DW that
         * stores by index; MI_LOAD_REGISTER_IMM of two registers. */
        const uint32_t cs[] = {MI_INSTR(FAKE_MI_FLUSH_DW, 1) | FAKE_POST_SYNC_WRITE |
                                   FAKE_STORE_INDEX,
                               I915_GEM_HWS_SEQNO_ADDR | 4u,
                               91u,
                               MI_INSTR(FAKE_MI_LOAD_REGISTER_IMM, 3),
                               0x2000u,
                               1u,
                               0x2004u,
                               2u,
                               MI_USER_INTERRUPT,
                               MI_NOOP};
        i915_engine_irq_get(bound, bcs);
        emit(bcs, cs, 10);
        CHECK("flush_dw: stored by index", bcs->hws[I915_GEM_HWS_SEQNO] == 91u);
        CHECK("lri: passed over by its length", f->ring[1].commands == 4 && !f->ring[1].hung);
        CHECK("bcs: its own interrupt taken", bcs->irq_seen == 1);
        i915_engine_irq_put(bound, bcs);
    }
    {
        /* A command of type 1, which no engine has. */
        const uint32_t cs[] = {0x20000000u, MI_NOOP};
        before = f->ring[0].commands;
        emit(rcs, cs, 2);
        CHECK("unknown: the ring hangs on it",
              f->ring[0].hung && R(f, RING_IPEHR(RENDER_RING_BASE)) == 0x20000000u &&
                  f->ring[0].commands == before && logged("fake: rcs0: hung at"));
    }
    i915_gt_pages_unmap(bound, page, 1, ggtt);
    done();
}

/** The file through its operations: open, read, write, seek and close,
 * with the device held while it is open. */
static void row_file(void)
{
    struct i915_fake *f = fresh(0);
    struct open_file ofd;
    fd_entry_t e;
    char buf[600];
    int32_t n;
    uint32_t refs;

    if (!f) {
        return;
    }
    k_memset(&ofd, 0, sizeof(ofd));
    k_memset(&e, 0, sizeof(e));
    e.ofd = &ofd;
    e.ops = dfs_ops;
    e.priv = dfs_data;
    refs = registered->refs;
    CHECK("file: opens", dfs_open(&e, 0) == 0 && e.priv != f && registered->refs == refs + 1);
    n = dfs_ops->read(&e, (char __user *)buf, sizeof(buf) - 1);
    CHECK("file: reads its text", n > 0);
    if (n > 0) {
        buf[n] = 0;
        CHECK("file: the text",
              strstr(buf, "fault hang armed 0 injected 0\n") && strstr(buf, "light never\n"));
    }
    CHECK("file: at its end", dfs_ops->read(&e, (char __user *)buf, sizeof(buf)) == 0);
    CHECK("file: writes",
          dfs_ops->write(&e, (const char __user *)"hang 2\n", 7) == 7 && f->armed[FAKE_HANG] == 2);
    CHECK("file: refuses", dfs_ops->write(&e, (const char __user *)"nonsense", 8) == -EINVAL);
    CHECK("file: refuses a long write",
          dfs_ops->write(&e, (const char __user *)buf, 64) == -EINVAL);
    CHECK("file: seeks", dfs_ops->llseek(&e, 0, 0) == 0);
    ofd.offset = 0;
    n = dfs_ops->read(&e, (char __user *)buf, sizeof(buf) - 1);
    if (n > 0) {
        buf[n] = 0;
    }
    CHECK("file: the text again after the write",
          n > 0 && strstr(buf, "fault hang armed 2 injected 0\n"));
    dfs_ops->close(&e);
    CHECK("file: the close lets the device go", registered->refs == refs && e.priv == 0);
    CHECK("file: cleared", cmd(f, "clear") == 0 && f->armed[FAKE_HANG] == 0);
    done();
}

/** Nothing: one thread has no one to give way to. */
void sched_give_way(void)
{}

int main(void)
{
    row_probe();
    row_no_edu();
    row_registers();
    row_light();
    row_edu_message();
    row_fault_submit();
    row_fault_bind();
    row_fault_irq();
    row_fault_hang();
    row_fault_commands();
    row_commands();
    row_file();
    CHECK("nothing left on the heap", heap_live == 0 && pages_live == 0);
    if (fails) {
        printf("i915-fake-test: %d failures\n", fails);
        return 1;
    }
    printf("i915-fake-test: ok\n");
    return 0;
}
