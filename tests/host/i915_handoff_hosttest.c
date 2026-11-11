/*
 * i915_handoff_hosttest.c: the Intel graphics driver's release of the HD
 * 4400 to another driver and its bring-up again from a reset, the path of
 * the Lenovo's step of 82-4b, run whole against the host model of the chip.
 *
 * host-deps: rach2/sys/drm/i915/i915_pci.c rach2/sys/drm/i915/i915_handoff.c
 * host-deps: rach2/sys/drm/i915/i915_reset.c rach2/sys/drm/i915/i915_irq.c
 * host-deps: tests/host/i915_model.c rach2/sys/display/backlight.c
 * host-deps: rach2/sys/display/intelbl.c rach2/sys/lib/kstring.c
 * host-deps: rach2/sys/lib/kstd.c
 * host-deps: rach2/sys/lib/kctypes.c tests/host/compat/spinlock_host.c
 * host-deps: rach2/sys/drm/i915/intel_bios.c rach2/sys/drm/i915/i915_power.c
 * host-deps: rach2/sys/drm/i915/i915_uncore.c rach2/sys/drm/i915/i915_stolen.c
 * host-deps: rach2/sys/drm/i915/i915_gtt.c rach2/sys/drm/i915/i915_ring.c
 * host-deps: rach2/sys/drm/i915/i915_ppgtt.c
 * host-deps: rach2/sys/drm/i915/i915_firstlight.c
 * host-deps: rach2/sys/mm/frame.c rach2/sys/tick.c
 * host-deps: rach2/sys/drm/i915/i915_rc6.c rach2/sys/drm/i915/i915_rps.c
 * host-deps: rach2/sys/drm/i915/intel_opregion.c
 * host-deps: rach2/sys/drm/i915/i915_aperture.c rach2/sys/drm/i915/i915_fence.c
 * host-deps: tests/host/compat/resv_host.c
 * host-deps: tests/host/compat/i915_gem_lock_host.c
 * host-deps: tests/host/compat/mutex_none.c
 * host-inputs: tests/fixtures/lenovo-gpu/i915_opregion.bin
 *
 * The whole driver stands on the model: the bind of i915_pci.c with its
 * base, rings and interrupts, as i915_trial_hosttest.c runs it, and the
 * display's files over drm_kms_model.c, as i915_kms_trial_hosttest.c runs
 * them. Around it are the kernel's parts the handoff uses, each a record
 * of the test's: the entries of /sys, through whose `unbind` and `bind`
 * every release and bring-up here goes; the PCI function's owner, command
 * word, message and reset; the DRM core's devices; the console's client;
 * and the boot's backlight driver. i915_hw.c is included whole so that the
 * registers the probe maps are the model's too, seen through a boundary
 * that counts every access and notes the writes the rows ask about.
 *
 * The function's reset is the test's: the display engine's and the PCH's
 * registers cleared but for the fuses' straps, the strap of the display on
 * DDI A, the raw clock and the PWM's granularity; DDI A's four lanes,
 * which the firmware sets, cleared with the rest; every interrupt masked,
 * the message and bus mastering off, the power well off with no request,
 * the pipes, the link and the panel down with the sequencer's count of the
 * power cycle lost, the GPU reset, and the GTT's entries left as they
 * were, since they are memory. The reference run on the Lenovo
 * (dev-log/76-graphics/research/run2/step11.md) gives what is known of the
 * chip after its reset: the panel dark, ASLS kept, and a backlight of 938
 * levels where the firmware's had 937, which is the PWM without its period
 * and with its granularity: 24 MHz over 200 Hz's 128 steps.
 *
 * The rows: the bind as the firmware leaves the chip, with the driver
 * shown in /sys; the refusals of both files; a release refused while a
 * program holds a window of the aperture, which leaves the panel lit; the
 * release, with the display taken down in Linux's order, the GPU reset
 * before the rings' pages go back, every interrupt masked and the message
 * off, bus mastering off, the function without an owner, the console told
 * that its screen has gone before it leaves the device, the backlight on a
 * stand-in, and a record that a program still holds reaching no register;
 * a bring-up refused while vfio-pci has the function or its
 * driver_override names another driver; the function scribbled on as a
 * guest's driver would leave it; a bring-up whose reset fails, and one
 * whose bind fails, each leaving the function without an owner; the
 * bring-up, which resets the function, waits out the panel's power cycle
 * and binds with nothing adopted, none of the guest's entries in the GTT
 * and the PWM's period from the VBT; a full mode set of the panel by
 * SETCRTC, with the panel powered, the link trained and frames counted;
 * the release, the bring-up and the mode set a second time, from a lit
 * panel that a master holds; and a GPU that will not reset, which refuses
 * the release and leaves the driver bound and the GPU wedged until it
 * does. The rows that expect no failure also find no wait at its bound.
 *
 * Returns 0 when every row passes, 1 otherwise.
 */

#include <stdarg.h>

#define KMS_MODEL_KERNEL
#define KMS_MODEL_DEVICES
#define KMS_MODEL_CLIENT
#include "drm_kms_model.c"
#include "drm/i915/intel_display.c"
#include "drm/i915/intel_plane.c"
#include "drm/i915/intel_cursor.c"
#include "drm/i915/intel_vblank.c"
#include "drm/i915/i915_display_debugfs.c"
#include "drm/i915/intel_backlight.c"
#include "drm/i915/intel_cdclk.c"
#include "drm/i915/intel_pps.c"
#include "drm/i915/intel_dp_aux.c"
#include "drm/i915/intel_gmbus.c"
#include "drm/i915/intel_dpll.c"
#include "drm/i915/intel_hdmi.c"
#include "drm/i915/intel_hotplug.c"
#include "drm/i915/intel_ddi.c"
#include "drm/i915/intel_dp_link_training.c"
#include "drm/i915/intel_dp.c"
#include "drm/i915/intel_panel.c"
#include "drm/i915/intel_pfit.c"
#include "drm/i915/intel_wm.c"
/* The probe's mapping of the registers is the test's, below. */
#define i915_hw_mmio_init i915_hw_mmio_init_chip
#include "drm/i915/i915_hw.c"
#undef i915_hw_mmio_init
#include "display/fbcon.h"
#include "fs/synthfs.h"
#include "fs/sysfs.h"
#include "iommu.h"
#include "irq.h"
#include "mm/paging.h"
#include "i915_model.h"
#include "i915_model_base.h"
#include "i915_model_ddi.h"
#include "i915_model_pipe.h"
#include "i915_model_reset.h"
#include "i915_model_ring.h"
#include "i915_model_sink.h"
#include "i915_lenovo.h"

extern int vsnprintf(char *s, unsigned long n, const char *format, va_list ap);
extern char *strstr(const char *s, const char *t);
extern int strncmp(const char *s, const char *t, unsigned long n);
extern int posix_memalign(void **p, unsigned long align, unsigned long size);
extern int open(const char *path, int flags, ...);
extern long read(int fd, void *buf, unsigned long n);
extern int close(int fd);

/* The chip's name on the bus, as /sys has it. */
#define CHIP "0000:00:02.0"

/* ---- the kernel around the driver ---- */

static struct i915_model model;
static char log_text[65536];

/** The boot log, kept whole for the rows that read it, from its start
 * again when full. */
void klog(int level, const char *subsystem, const char *format, ...)
{
    unsigned long n = strlen(log_text);
    va_list ap;

    (void)level;
    (void)subsystem;
    if (n > sizeof(log_text) - 512) {
        n = 0;
    }
    va_start(ap, format);
    vsnprintf(log_text + n, sizeof(log_text) - n - 1, format, ap);
    va_end(ap);
    n = strlen(log_text);
    log_text[n] = '\n';
    log_text[n + 1] = 0;
}

/** Whether the log holds `text`. */
static int logged(const char *text)
{
    return strstr(log_text, text) != 0;
}

/** Whether the log holds a wait of the driver that reached its bound. */
static int timed_out(void)
{
    return logged("timed out") || logged("timeout") || logged("no vblank in 100 ms") ||
           logged("not ready");
}

/** The latest bytes of the driver's log, for a row that failed. */
static void show_log(void)
{
    unsigned long n = strlen(log_text);

    printf("%s", n > 6000 ? log_text + n - 6000 : log_text);
}

uint32_t kirq_save(void)
{
    return 0;
}

void kirq_restore(uint32_t flags)
{
    (void)flags;
}

/** The scheduler, which never runs here: the driver's sleeps are waits on
 * the model's clock. */
int sched_is_enabled(void)
{
    return 0;
}

static int64_t clock_base_ns; /* the monotonic time at the model's 0 */

/** After each wait of the driver: the monotonic clock follows the
 * model's. */
static void follow_clock(struct i915_model *m)
{
    model_now_ns = clock_base_ns + (int64_t)m->now_us * 1000;
}

/** A spin of the kernel's, as the handoff's sleeps are without a
 * scheduler: the model's clock moves. */
void k_udelay(uint32_t usec)
{
    model.now_us += usec;
    follow_clock(&model);
}

static int opt_i915 = 1;

int kcmdline_has(const char *flag)
{
    return opt_i915 && strcmp(flag, "rach2.i915") == 0;
}

/** The fake chip, which i915_fake_hosttest.c tests and no option here
 * names. */
int32_t i915_fake_probe(void)
{
    return 0;
}

int32_t i915_fake_bind(const pci_device_t *pdev)
{
    (void)pdev;
    return -ENODEV;
}

/* ---- the firmware's memory: the Lenovo's OpRegion at ASLS ---- */

static uint8_t opregion[OPREGION_SIZE];

/** The OpRegion's bytes for a map of them, 0 for any other firmware
 * memory, the option ROM's shadow among them. */
uintptr_t mmio_map(uint64_t phys, size_t size, pte_t cache)
{
    (void)cache;
    if (phys >= LENOVO_GPU_ASLS && phys + size <= (uint64_t)LENOVO_GPU_ASLS + sizeof(opregion)) {
        return (uintptr_t)(opregion + (phys - LENOVO_GPU_ASLS));
    }
    return 0;
}

void mmio_unmap(uintptr_t va, size_t size)
{
    (void)va;
    (void)size;
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

int pat_write_combining(void)
{
    return 1;
}

/** The MMIO backend's own mapping, which no row reaches: the probe's
 * `i915_hw_mmio_init` is the test's. */
uintptr_t map_mmio_typed(uintptr_t phys_addr, size_t size, pte_t cache)
{
    (void)phys_addr;
    (void)size;
    (void)cache;
    return 0;
}

/* ---- memory: the heap's pages, each with a physical address ---- */

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
    uint32_t i, k;

    if (posix_memalign(&block, 4096, (unsigned long)count * 4096u) != 0) {
        return 0;
    }
    for (i = 0; i < count; i++) {
        for (k = 0; k < (uint32_t)npages && pages[k].host; k++) {
        }
        if (k == TEST_PAGES) {
            free(block);
            return 0;
        }
        if (k == (uint32_t)npages) {
            npages++;
        }
        pages[k].host = (uint8_t *)block + i * 4096u;
        pages[k].phys = next_phys;
        next_phys += 0x2000u;
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

void *kmap_temp(uintptr_t phys)
{
    (void)phys;
    return 0;
}

void kunmap_temp(void *va)
{
    (void)va;
}

/* VT-d as intel_iommu=off leaves it: the RMRR for the GPU named, and no
 * unit translating. */
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

/* ---- the PCI function: configuration space, owner, message, reset ---- */

static const char *owner; /* the function's, or NULL */
static int vectors_live, resets, reset_error;
static uint64_t reset_at_us; /* the model's time of the last reset */
static int reset_owner_bad;  /* a reset ran while the function had an owner */
static void model_function_reset(void);

/** Whether (bus, device, function) is the model's chip. */
static int is_chip(uint8_t b, uint8_t d, uint8_t f)
{
    return b == LENOVO_GPU_BUS && d == LENOVO_GPU_DEVICE && f == LENOVO_GPU_FUNCTION;
}

uint32_t pci_read_config32(uint8_t b, uint8_t d, uint8_t f, uint8_t offset)
{
    return is_chip(b, d, f) ? i915_model_config(&model, offset, 4) : 0xffffffffu;
}

uint16_t pci_read_config16(uint8_t b, uint8_t d, uint8_t f, uint8_t offset)
{
    return is_chip(b, d, f) ? (uint16_t)i915_model_config(&model, offset, 2) : 0xffffu;
}

void pci_write_config16(uint8_t b, uint8_t d, uint8_t f, uint8_t offset, uint16_t value)
{
    if (is_chip(b, d, f)) {
        model.config[offset] = (uint8_t)value;
        model.config[offset + 1] = (uint8_t)(value >> 8);
    }
}

/** The chip's command word. */
static uint16_t command(void)
{
    return pci_read_config16(0, 2, 0, PCI_COMMAND);
}

/** Set `bits` in the command word, which makes the kernel the function's
 * owner when it has none, as pci.c's enables do. */
static void command_set(const pci_device_t *dev, uint16_t bits)
{
    pci_write_config16(dev->bus, dev->device, dev->function, PCI_COMMAND,
                       (uint16_t)(command() | bits));
    if (!owner) {
        owner = PCI_OWNER_KERNEL;
    }
}

void pci_enable_bus_master(pci_device_t *dev)
{
    command_set(dev, PCI_CMD_BUS_MASTER);
}

void pci_enable_memory_space(pci_device_t *dev)
{
    command_set(dev, PCI_CMD_MEMORY_SPACE);
}

void pci_disable_bus_master(pci_device_t *dev)
{
    pci_write_config16(dev->bus, dev->device, dev->function, PCI_COMMAND,
                       (uint16_t)(command() & ~PCI_CMD_BUS_MASTER));
}

int pci_claim(uint8_t b, uint8_t devfn, const char *who)
{
    if (!is_chip(b, (uint8_t)(devfn >> 3), devfn & 7u)) {
        return -ENOSPC;
    }
    if (owner && strcmp(owner, who) != 0) {
        return -EBUSY;
    }
    owner = who;
    return 0;
}

void pci_unclaim(uint8_t b, uint8_t devfn, const char *who)
{
    if (is_chip(b, (uint8_t)(devfn >> 3), devfn & 7u) && owner && strcmp(owner, who) == 0) {
        owner = 0;
    }
}

const char *pci_owner(uint8_t b, uint8_t devfn)
{
    return is_chip(b, (uint8_t)(devfn >> 3), devfn & 7u) ? owner : 0;
}

bool pci_nth(int nth, pci_device_t *out)
{
    if (nth != 0) {
        return false;
    }
    *out = model.pci;
    return true;
}

bool pci_find_nth_by_id(uint16_t vendor_id, uint16_t device_id, int nth, pci_device_t *out)
{
    if (nth == 0 && model.pci.vendor_id == vendor_id && model.pci.device_id == device_id) {
        *out = model.pci;
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
    model.config[LENOVO_GPU_CAP_MSI + 2] |= 1u;
    return 0;
}

void pci_msi_disable(const pci_device_t *dev)
{
    if (is_chip(dev->bus, dev->device, dev->function)) {
        model.config[LENOVO_GPU_CAP_MSI + 2] &= (uint8_t)~1u;
    }
}

/** Whether the chip's message is enabled. */
static int msi_on(void)
{
    return model.config[LENOVO_GPU_CAP_MSI + 2] & 1u;
}

/** The chip's method: its Advanced Features capability's reset. */
int pci_reset_method(const pci_device_t *dev)
{
    (void)dev;
    return PCI_RESET_AF_FLR;
}

/** The function's reset as pci.c runs it, with the header kept: the test's
 * reset of the model after the 100 ms the function is given, slept through
 * the caller's sleep; or the error a row asks for, with nothing reset. */
int pci_reset_function(const pci_device_t *dev, pci_sleep_t msleep)
{
    (void)dev;
    resets++;
    if (owner) {
        reset_owner_bad++;
    }
    if (reset_error) {
        return reset_error;
    }
    model_function_reset();
    msleep(100);
    return 0;
}

int isr_msi_ready(void)
{
    return 1;
}

int isr_alloc_vector(void)
{
    vectors_live++;
    return 48;
}

void isr_free_vector(int vector)
{
    (void)vector;
    vectors_live--;
}

static isr_handler_t handler;
static uint8_t handler_vector;

void register_isr_handler(uint8_t vector, isr_handler_t h, const char *name)
{
    (void)name;
    handler = h;
    handler_vector = vector;
}

/* ---- /sys: the entries drivers add, as fs/sysfs.h has them ---- */

#define SYS_NODES 32
static struct sys_node {
    int used;
    int parent; /* a handle, or 0 for an anchor */
    int kind;   /* SYSFS_NODE_* */
    char name[SYSFS_NAME_MAX];
    int node, key; /* an anchor's */
    const struct sysfs_attr *attr;
    void *obj;
    int arg;
    int target;
} sys[SYS_NODES];

/** A free slot's handle, from 1, or -ENOSPC. */
static int sys_new(int parent, int kind, const char *name)
{
    int i;

    for (i = 0; i < SYS_NODES; i++) {
        if (!sys[i].used) {
            k_memset(&sys[i], 0, sizeof(sys[i]));
            sys[i].used = 1;
            sys[i].parent = parent;
            sys[i].kind = kind;
            k_strncpy(sys[i].name, name, SYSFS_NAME_MAX - 1);
            return i + 1;
        }
    }
    return -ENOSPC;
}

int sysfs_anchor_find(int node, int key)
{
    int i;

    for (i = 0; i < SYS_NODES; i++) {
        if (sys[i].used && sys[i].kind == SYSFS_NODE_ANCHOR && sys[i].node == node &&
            sys[i].key == key) {
            return i + 1;
        }
    }
    return 0;
}

int sysfs_anchor(int node, int key)
{
    int h = sysfs_anchor_find(node, key);

    if (h == 0 && (h = sys_new(0, SYSFS_NODE_ANCHOR, "")) > 0) {
        sys[h - 1].node = node;
        sys[h - 1].key = key;
    }
    return h;
}

int sysfs_node_child(int dir, const char *name, int len)
{
    int i;

    for (i = 0; i < SYS_NODES; i++) {
        if (sys[i].used && sys[i].parent == dir && dir > 0 && (int)strlen(sys[i].name) == len &&
            strncmp(sys[i].name, name, (unsigned long)len) == 0) {
            return i + 1;
        }
    }
    return 0;
}

/** Add an entry of `kind` named `name` below `parent`, as the registry's
 * adds do: its handle, -ENOENT for no such directory, -EEXIST for a name
 * taken. */
static int sys_add(int parent, int kind, const char *name)
{
    if (parent <= 0 || parent > SYS_NODES || !sys[parent - 1].used ||
        (sys[parent - 1].kind != SYSFS_NODE_DIR && sys[parent - 1].kind != SYSFS_NODE_ANCHOR)) {
        return -ENOENT;
    }
    if (sysfs_node_child(parent, name, (int)strlen(name))) {
        return -EEXIST;
    }
    return sys_new(parent, kind, name);
}

int sysfs_add_dir(int parent, const char *name)
{
    return sys_add(parent, SYSFS_NODE_DIR, name);
}

int sysfs_add_attr(int parent, const struct sysfs_attr *attr, void *obj, int arg)
{
    int h = sys_add(parent, SYSFS_NODE_ATTR, attr->name);

    if (h > 0) {
        sys[h - 1].attr = attr;
        sys[h - 1].obj = obj;
        sys[h - 1].arg = arg;
    }
    return h;
}

int sysfs_add_link(int parent, const char *name, int target)
{
    int h = sys_add(parent, SYSFS_NODE_LINK, name);

    if (h > 0) {
        sys[h - 1].target = target;
    }
    return h;
}

void sysfs_remove(int handle)
{
    int i;

    if (handle <= 0 || handle > SYS_NODES || !sys[handle - 1].used ||
        sys[handle - 1].kind == SYSFS_NODE_ANCHOR) {
        return;
    }
    sys[handle - 1].used = 0;
    for (i = 0; i < SYS_NODES; i++) {
        if (sys[i].used && sys[i].parent == handle) {
            sysfs_remove(i + 1);
        }
    }
}

/* The one binding `sysfs_pci_bind` shows here, as fs/sysfs.c makes it: the
 * function's key, or -1, its `driver` link and the directory's link back. */
static int bound_key = -1, bound_link, bound_back;

int sysfs_pci_bind(int key, const char *driver)
{
    static const char hex[] = "0123456789abcdef";
    char name[SYSFS_NAME_MAX];
    int dev = sysfs_anchor(SYSFS_PCI_DEVICE, key);
    int dir = sysfs_anchor(SYSFS_BUS_PCI, 0), h;

    if (bound_key >= 0) {
        return bound_key == key ? 0 : -ENOSPC;
    }
    h = sysfs_node_child(dir, "drivers", 7);
    dir = h > 0 ? h : sysfs_add_dir(dir, "drivers");
    h = sysfs_node_child(dir, driver, (int)strlen(driver));
    dir = h > 0 ? h : sysfs_add_dir(dir, driver);
    k_memcpy(name, "0000:00:00.0", 13);
    name[5] = hex[(key >> 12) & 15];
    name[6] = hex[(key >> 8) & 15];
    name[8] = hex[(key >> 7) & 1];
    name[9] = hex[(key >> 3) & 15];
    name[11] = hex[key & 7];
    bound_link = sysfs_add_link(dev, "driver", dir);
    bound_back = sysfs_add_link(dir, name, dev);
    bound_key = key;
    return bound_link > 0 && bound_back > 0 ? 0 : -ENOENT;
}

void sysfs_pci_unbind(int key)
{
    if (key == bound_key) {
        sysfs_remove(bound_link);
        sysfs_remove(bound_back);
        bound_key = -1;
    }
}

int32_t sysfs_node_show(int handle, char *buf, uint32_t cap)
{
    struct sys_node *n = handle > 0 && handle <= SYS_NODES ? &sys[handle - 1] : 0;

    if (!n || !n->used || n->kind != SYSFS_NODE_ATTR) {
        return -ENOENT;
    }
    return n->attr->show ? n->attr->show(n->obj, n->arg, buf, cap) : -EACCES;
}

/** The card's directory in /sys: none, so the driver adds no files of the
 * frequency's there. */
int drm_sysfs_card_dir(const struct drm_device *dev)
{
    (void)dev;
    return -ENOENT;
}

/** The handle of the path below /sys/bus/pci, "drivers/i915/bind", or 0. */
static int sys_bus_path(const char *a, const char *b, const char *c)
{
    int h = sysfs_anchor_find(SYSFS_BUS_PCI, 0);

    h = h > 0 && a ? sysfs_node_child(h, a, (int)strlen(a)) : h;
    h = h > 0 && b ? sysfs_node_child(h, b, (int)strlen(b)) : h;
    h = h > 0 && c ? sysfs_node_child(h, c, (int)strlen(c)) : h;
    return h;
}

/** A root's write of `text` to the driver's file `file`, bind or unbind:
 * what the write returns, the text's length or -errno. */
static int32_t driver_write(const char *file, const char *text)
{
    int h = sys_bus_path("drivers", "i915", file);

    if (h <= 0 || !sys[h - 1].attr->store) {
        return -ENOENT;
    }
    return sys[h - 1].attr->store(sys[h - 1].obj, sys[h - 1].arg, text, (uint32_t)strlen(text));
}

/** Whether /sys shows the driver on the chip: the function's `driver`
 * link to the driver's directory and the directory's link to the
 * function. */
static int sys_bound(void)
{
    int dir = sys_bus_path("drivers", "i915", 0);
    int pci = sysfs_anchor_find(SYSFS_PCI_DEVICE, LENOVO_GPU_DEVICE << 3);
    int a = pci > 0 ? sysfs_node_child(pci, "driver", 6) : 0;
    int b = dir > 0 ? sysfs_node_child(dir, CHIP, 12) : 0;

    return a > 0 && b > 0 && sys[a - 1].kind == SYSFS_NODE_LINK && sys[a - 1].target == dir &&
           sys[b - 1].kind == SYSFS_NODE_LINK && sys[b - 1].target == pci;
}

/* The function's driver_override, as VFIO adds it: "" for none. */
static const char *override = "";

/** driver_override: the driver named, or "(null)" for none. */
static int32_t show_override(void *obj, int arg, char *buf, uint32_t cap)
{
    const char *s = override[0] ? override : "(null)";
    uint32_t n = (uint32_t)strlen(s);

    (void)obj;
    (void)arg;
    if (n + 1 <= cap) {
        k_memcpy(buf, s, n);
        buf[n] = '\n';
    }
    return (int32_t)(n + 1);
}

static const struct sysfs_attr attr_override = {"driver_override", 0644u, show_override, 0};

/* ---- the console's client and the loader's framebuffer ---- */

/* The loader's framebuffer as the Lenovo's firmware leaves it: at the
 * start of the aperture. */
static struct boot_framebuffer boot_screen;
bool fbcon_active = true;

const struct boot_framebuffer *fbcon_boot_screen(void)
{
    return &boot_screen;
}

/* What the console was told, in order: 'r' a device registered, 'g' the
 * loader's framebuffer gone, 'u' a device left, 'h' its outputs changed. */
static char console_calls[64];

/** Note a call of the console's. */
static void console_note(char c)
{
    unsigned long n = strlen(console_calls);

    if (n + 1 < sizeof(console_calls)) {
        console_calls[n] = c;
    }
}

void drm_client_dev_register(struct drm_device *dev)
{
    (void)dev;
    console_note('r');
}

void drm_client_dev_unregister(struct drm_device *dev)
{
    (void)dev;
    console_note('u');
}

void drm_client_dev_hotplug(struct drm_device *dev)
{
    (void)dev;
    console_note('h');
}

void drm_client_boot_screen_gone(void)
{
    console_note('g');
    fbcon_active = false;
}

/** Forget what the console was told. */
static void console_clear(void)
{
    k_memset(console_calls, 0, sizeof(console_calls));
}

/* ---- the DRM core's devices ---- */

static struct drm_device *cards[4];
static int releases; /* records freed */

void drm_dev_init(struct drm_device *dev, const struct drm_driver *driver)
{
    k_memset(dev, 0, sizeof(*dev));
    dev->driver = driver;
    dev->features = driver->ident.features;
    dev->index = -1;
    dev->refs = 1;
    mutex_init(&dev->mode_lock, "drm_mode_lock");
}

void drm_dev_get(struct drm_device *dev)
{
    dev->refs++;
}

void drm_dev_put(struct drm_device *dev)
{
    if (!dev || --dev->refs > 0) {
        return;
    }
    if (dev->index >= 0 && cards[dev->index] == dev) {
        cards[dev->index] = NULL;
    }
    releases++;
    if (dev->driver->release) {
        dev->driver->release(dev);
    }
}

static int register_error; /* what the next registration fails with, or 0 */

int32_t drm_dev_register(struct drm_device *dev)
{
    int i;

    if (register_error) {
        i = register_error;
        register_error = 0;
        return i;
    }
    for (i = 0; i < 4 && cards[i]; i++) {
    }
    if (i == 4) {
        return -ENOSPC;
    }
    dev->index = i;
    cards[i] = dev;
    dev->registered = 1;
    k_strncpy(dev->unique, "pci:" CHIP, sizeof(dev->unique) - 1);
    drm_client_dev_register(dev);
    return 0;
}

/** As drm.c's, less the nodes and entries this kernel has not: the console
 * leaves, the vblanks' waiters go, and the driver's unplug runs. */
void drm_dev_unplug(struct drm_device *dev)
{
    if (dev->unplugged) {
        return;
    }
    dev->unplugged = 1;
    if (dev->registered) {
        drm_client_dev_unregister(dev);
    }
    if (drm_core_check_feature(dev, DRIVER_MODESET)) {
        drm_vblank_unplug(dev);
    }
    if (dev->driver->unplug) {
        dev->driver->unplug(dev);
    }
    drm_dev_put(dev);
}

/** The registered device, the one the driver has now, or NULL. */
static struct drm_device *card(void)
{
    int i;

    for (i = 0; i < 4; i++) {
        if (cards[i] && cards[i]->registered && !cards[i]->unplugged) {
            return cards[i];
        }
    }
    return NULL;
}

static int aperture_removals;

int32_t drm_aperture_remove_conflicting(uint64_t base, uint64_t size, const char *name)
{
    (void)base;
    (void)size;
    (void)name;
    aperture_removals++;
    return 0;
}

/* ---- the driver's buffers, contexts and requests, which the bind and the
 * release reach only through these ---- */

const struct drm_ioctl_desc i915_ioctls[I915_NUM_IOCTLS];
const struct drm_gem_object_funcs i915_gem_object_funcs;

int32_t i915_gem_dumb_create(struct drm_file *file, struct drm_device *dev,
                             struct drm_mode_create_dumb *args)
{
    (void)dev;
    (void)file;
    (void)args;
    return -ENODEV;
}

struct drm_gem_object *i915_gem_prime_import(struct drm_device *dev, struct dma_buf *buf,
                                             int32_t *err)
{
    (void)dev;
    (void)buf;
    *err = -ENODEV;
    return 0;
}

static int gem_unplugs, request_unplugs;
static uint32_t rings_running_at_unplug; /* the rings enabled when the requests' unplug ran */

void i915_gem_unplug(struct drm_i915_private *i915)
{
    (void)i915;
    gem_unplugs++;
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

/** The requests' unplug, the first thing the driver's unplug does: note
 * whether the chip still had a ring enabled then. */
void i915_requests_unplug(struct drm_i915_private *i915)
{
    static const uint32_t bases[] = {RENDER_RING_BASE, BLT_RING_BASE, GEN6_BSD_RING_BASE,
                                     VEBOX_RING_BASE};
    unsigned i;

    (void)i915;
    request_unplugs++;
    rings_running_at_unplug = 0;
    for (i = 0; i < 4; i++) {
        if (i915_model_peek(&model, RING_CTL(bases[i])) & RING_VALID) {
            rings_running_at_unplug |= 1u << i;
        }
    }
}

/* The reset of a hung GPU, i915_reset.c's other half, which
 * i915_reset_hosttest.c tests: no ring here carries a request. */
uint16_t i915_engine_uabi_class(const struct i915_engine *engine)
{
    (void)engine;
    return 0;
}

int i915_gem_context_mark_guilty(struct intel_context *ce, uint64_t now)
{
    (void)ce;
    (void)now;
    return 0;
}

void i915_gem_context_mark_innocent(struct intel_context *ce)
{
    (void)ce;
}

void i915_gpu_error_capture(struct drm_i915_private *i915, uint32_t engine_mask, const char *reason)
{
    (void)i915;
    (void)engine_mask;
    (void)reason;
}

void i915_request_set_error_once(struct i915_request *rq, int32_t err)
{
    (void)rq;
    (void)err;
}

void i915_request_skip(struct i915_request *rq)
{
    (void)rq;
}

void i915_requests_cancel(struct drm_i915_private *i915)
{
    (void)i915;
}

void i915_vma_revoke_mmap(struct i915_vma *vma)
{
    (void)vma;
}

/** Make `obj` a buffer object of `dev`, held once, as drm_gem.c does. */
void drm_gem_object_init(struct drm_device *dev, struct drm_gem_object *obj, uint64_t size,
                         const struct drm_gem_object_funcs *funcs)
{
    obj->dev = dev;
    obj->resv = &obj->resv_own;
    obj->size = size;
    obj->funcs = funcs;
    obj->refs = 1;
}

/** The file's tables are zeroed and never shared here: nothing to make. */
void drm_file_locks_init(struct drm_file *file)
{
    (void)file;
}

/* The master's buffer objects: one a size, bound in the aperture by the
 * pin. */
#define NBO 4
static struct drm_i915_gem_object bo[NBO];
static struct i915_vma bo_vma[NBO];

/** The GGTT address the pin binds object `i` at. */
#define BO_GGTT(i) (0x01000000u + (uint32_t)(i) * 0x01000000u)

int32_t i915_gem_object_pin_to_display(struct drm_i915_gem_object *obj,
                                       const struct i915_remap_info *remap, uint32_t align,
                                       struct i915_vma **vma)
{
    struct i915_vma *v = &bo_vma[obj - bo];

    (void)remap;
    (void)align;
    if (!obj->vmas) {
        v->obj = obj;
        v->start = BO_GGTT(obj - bo);
        v->mappable = 1;
        obj->vmas = v;
    }
    v->pin_count++;
    *vma = v;
    return 0;
}

void i915_vma_unpin(struct i915_vma *vma)
{
    CHECK(vma->pin_count > 0, "a binding is unpinned no more than it is pinned");
    vma->pin_count--;
}

void i915_gem_object_flush_range(struct drm_i915_gem_object *obj, uint64_t start, uint64_t end)
{
    (void)obj;
    (void)start;
    (void)end;
}

/* ---- the boot's backlight driver ---- */

static int boot_level = 937, boot_gets, boot_sets, boot_has;

/** The boot driver's `get`, a read of the PCH's PWM on the machine. */
static int boot_get(void *ctx)
{
    (void)ctx;
    boot_gets++;
    return boot_level;
}

/** The boot driver's `set`, a write of the PCH's PWM on the machine. */
static void boot_set(void *ctx, int level)
{
    (void)ctx;
    boot_level = level;
    boot_sets++;
}

static const struct backlight_ops boot_ops = {boot_get, boot_set, NULL};

/** The boot's probe, which backlight.c's init calls and no row does. */
void intelbl_init(void)
{}

/** The boot's driver takes "intel_backlight" back when i915's display
 * goes, as intelbl.c's does on the Lenovo. */
int intelbl_take_back(void)
{
    boot_has = 1;
    return backlight_rebind("intel_backlight", 937, &boot_ops, NULL) < 0 ? -1 : 0;
}

/** A root's write of `text` to the backlight's attribute `attr`. */
static int bl_store(int attr, const char *text)
{
    return backlight_store(0, attr, text, (int)strlen(text));
}

/** The number the backlight's attribute `attr` shows, or -1. */
static int bl_show(int attr)
{
    char text[32];
    int n = backlight_show(0, attr, text, (int)sizeof(text)), v = 0, i;

    if (n <= 0) {
        return -1;
    }
    for (i = 0; i < n && text[i] >= '0' && text[i] <= '9'; i++) {
        v = v * 10 + (text[i] - '0');
    }
    return v;
}

/* ---- the boundary the driver sees ---- */

static const struct i915_hw_ops *model_ops;
static struct i915_hw chip_hw;   /* the model behind the boundary */
static uint32_t accesses;        /* reads and writes that reached the model */
static uint64_t first_access_us; /* the model's time of the first since the last reset */
static int first_access_seen;

/* The writes a row asks about, each the count of accesses at its first
 * since `marks_clear`, 0 for none: the backlight's enable cleared, the
 * eDP transcoder disabled, DDI A's buffer disabled, the panel's power
 * taken, the GPU's reset, the message's master enable cleared. */
static struct {
    uint32_t blc_off, trans_off, ddi_off, panel_off, gdrst, master_off, panel_on;
    uint64_t panel_on_us;
} mark;

/** Forget the marks. */
static void marks_clear(void)
{
    k_memset(&mark, 0, sizeof(mark));
}

/** Note the model's time of the first access after a reset. */
static void note_access(void)
{
    accesses++;
    if (!first_access_seen) {
        first_access_seen = 1;
        first_access_us = model.now_us;
    }
}

/** A read, counted, then passed on to the model. */
static uint32_t chip_read(struct i915_hw *hw, uint32_t reg)
{
    note_access();
    return model_ops->read32(hw, reg);
}

/** A write, counted and noted, then passed on to the model. */
static void chip_write(struct i915_hw *hw, uint32_t reg, uint32_t value)
{
    uint32_t old = i915_model_peek(&model, reg);

    note_access();
    if (reg == PP_CONTROL) {
        if ((old & EDP_BLC_ENABLE) && !(value & EDP_BLC_ENABLE) && !mark.blc_off) {
            mark.blc_off = accesses;
        }
        if ((old & PANEL_POWER_ON) && !(value & PANEL_POWER_ON) && !mark.panel_off) {
            mark.panel_off = accesses;
        }
        if (!(old & PANEL_POWER_ON) && (value & PANEL_POWER_ON) && !mark.panel_on) {
            mark.panel_on = accesses;
            mark.panel_on_us = model.now_us;
        }
    } else if (reg == TRANSCONF(I915_TRANS_EDP)) {
        if ((old & TRANSCONF_ENABLE) && !(value & TRANSCONF_ENABLE) && !mark.trans_off) {
            mark.trans_off = accesses;
        }
    } else if (reg == DDI_BUF_CTL(0)) {
        if ((old & DDI_BUF_CTL_ENABLE) && !(value & DDI_BUF_CTL_ENABLE) && !mark.ddi_off) {
            mark.ddi_off = accesses;
        }
    } else if (reg == GEN6_GDRST) {
        if ((value & GEN6_GRDOM_FULL) && !mark.gdrst) {
            mark.gdrst = accesses;
        }
    } else if (reg == DEIER) {
        if ((old & DE_MASTER_IRQ_CONTROL) && !(value & DE_MASTER_IRQ_CONTROL)) {
            mark.master_off = accesses;
        }
    }
    model_ops->write32(hw, reg, value);
}

/** A wait, passed on to the model, whose clock moves. */
static void chip_delay(struct i915_hw *hw, uint32_t us)
{
    model_ops->delay_us(hw, us);
}

static const struct i915_hw_ops chip_ops = {"model", chip_read, chip_write, chip_delay, 0};
static int probe_maps;

/** The probe's mapping of BAR 0's registers: the model behind the test's
 * boundary, which answers for the GTT's entries too. */
int32_t i915_hw_mmio_init(struct i915_hw *hw, uint64_t phys, uint32_t size)
{
    if (phys != LENOVO_GPU_BAR0 || size != I915_MMIO_SIZE) {
        return -ENOMEM;
    }
    probe_maps++;
    *hw = chip_hw;
    return 0;
}

/* ---- the chip ---- */

/** A register's value in the model's file. */
static uint32_t reg(uint32_t r)
{
    return i915_model_peek(&model, r);
}

/** Take every message the model has pending, as the processor calls the
 * handler of the vector. */
static void take_messages(void)
{
    isr_params_t params;

    while (i915_model_take_message(&model)) {
        if (handler) {
            k_memset(&params, 0, sizeof(params));
            params.vector = handler_vector;
            handler(&params);
        }
    }
}

/** One frame, 16.67 ms: a vblank of each pipe that is on and the messages
 * they raise, then each pipe's CRC done and those. */
static void frame(void)
{
    int pipe;

    model.now_us += 16667u;
    follow_clock(&model);
    for (pipe = 0; pipe < 3; pipe++) {
        i915_model_pipe_vblank(&model, pipe);
    }
    take_messages();
    for (pipe = 0; pipe < 3; pipe++) {
        i915_model_pipe_crc_done(&model, pipe);
    }
    take_messages();
}

/** What runs while a commit or a reader sleeps: frames, until the
 * condition holds, at most a second's. */
static void sleep_frames(wq_cond_fn cond, void *arg)
{
    int i;

    for (i = 0; i < 60 && !cond(arg); i++) {
        frame();
    }
}

/**
 * The function's reset, as the comment at the top says: what an owner
 * left in the display engine's and the PCH's registers is gone, the GPU is
 * reset, and the GTT's entries stay.
 */
static void model_function_reset(void)
{
    struct i915_model_fw *fw = i915_model_forcewake(&model);
    struct i915_model_pw *pw = i915_model_power(&model);
    struct i915_model_sink *sink = i915_model_sink(&model);
    struct i915_model_panel *panel = i915_model_panel(&model);
    uint32_t strap = reg(SFUSE_STRAP), fuse = reg(FUSE_STRAP), raw = reg(PCH_RAWCLK_FREQ);
    uint32_t chicken = reg(SOUTH_CHICKEN2) & LPT_PWM_GRANULARITY;
    uint32_t r;

    model_ops->write32(&model.hw, GEN6_GDRST, GEN6_GRDOM_FULL);
    fw->req = 0;
    fw->pending = 0;
    i915_model_poke(&model, FORCEWAKE_MT, 0);
    i915_model_poke(&model, FORCEWAKE_ACK_HSW, 0);
    for (r = 0x40000u; r < 0x80000u; r += 4) {
        i915_model_poke(&model, r, 0);
    }
    for (r = 0xc0000u; r < 0x100000u; r += 4) {
        i915_model_poke(&model, r, 0);
    }
    i915_model_poke(&model, SFUSE_STRAP, strap);
    i915_model_poke(&model, FUSE_STRAP, fuse);
    i915_model_poke(&model, PCH_RAWCLK_FREQ, raw);
    i915_model_poke(&model, DDI_BUF_CTL(0), DDI_INIT_DISPLAY_DETECTED);
    i915_model_poke(&model, SOUTH_CHICKEN2, chicken);
    i915_model_poke(&model, DEIMR, 0xffffffffu);
    i915_model_poke(&model, GTIMR, 0xffffffffu);
    i915_model_poke(&model, GEN6_PMIMR, 0xffffffffu);
    i915_model_poke(&model, SDEIMR, 0xffffffffu);
    i915_model_poke(&model, GEN6_RC_CONTROL, 0);
    i915_model_poke(&model, GEN6_RP_CONTROL, 0);
    i915_model_irq_state(&model)->line = 0;
    i915_model_irq_state(&model)->pending = 0;
    pw->req[0] = pw->req[1] = pw->req[2] = pw->req[3] = 0;
    pw->on = 0;
    pw->pending = 0;
    k_memset(i915_model_pipes(&model), 0, sizeof(struct i915_model_pipes));
    sink->trained = 0;
    sink->normal = 0;
    sink->powered = 0;
    sink->dpcd[DP_LINK_BW_SET] = 0;
    sink->dpcd[DP_LANE_COUNT_SET] = 0;
    sink->dpcd[DP_SET_POWER] = DP_SET_POWER_D0;
    panel->on = 0;
    panel->seq = 0;
    panel->left_us = 0;
    panel->cycle = 0;
    panel->cycle_us = 0;
    panel->up_pending = 0;
    model.config[LENOVO_GPU_CAP_MSI + 2] &= (uint8_t)~1u;
    model.config[PCI_COMMAND] &= (uint8_t)~PCI_CMD_BUS_MASTER;
    reset_at_us = model.now_us;
    first_access_seen = 0;
}

/** Whether every interrupt source of the chip is masked and none is
 * enabled, the master enable among them. */
static int interrupts_off(void)
{
    return reg(GTIMR) == 0xffffffffu && reg(DEIMR) == 0xffffffffu &&
           reg(GEN6_PMIMR) == 0xffffffffu && reg(SDEIMR) == 0xffffffffu && reg(DEIER) == 0 &&
           reg(GTIER) == 0 && reg(GEN6_PMIER) == 0 && reg(SDEIER) == 0 &&
           reg(RING_IMR(RENDER_RING_BASE)) == 0xffffffffu &&
           reg(RING_IMR(BLT_RING_BASE)) == 0xffffffffu &&
           reg(RING_IMR(GEN6_BSD_RING_BASE)) == 0xffffffffu &&
           reg(RING_IMR(VEBOX_RING_BASE)) == 0xffffffffu;
}

/** Whether no ring of the chip is enabled and each is empty. */
static int rings_idle(void)
{
    static const uint32_t bases[] = {RENDER_RING_BASE, BLT_RING_BASE, GEN6_BSD_RING_BASE,
                                     VEBOX_RING_BASE};
    unsigned i;

    for (i = 0; i < 4; i++) {
        if ((reg(RING_CTL(bases[i])) & RING_VALID) || (reg(RING_HEAD(bases[i])) & HEAD_ADDR) ||
            (reg(RING_TAIL(bases[i])) & TAIL_ADDR)) {
            return 0;
        }
    }
    return 1;
}

/** Whether the panel's output is down whole: the plane, the pipe, its
 * route and DDI A's buffer off, the panel without power and the PWM and
 * the backlight's enable off. */
static int display_off(void)
{
    return !(reg(DSPCNTR(I915_PIPE_A)) & DISP_ENABLE) &&
           !(reg(TRANSCONF(I915_TRANS_EDP)) & TRANSCONF_ENABLE) &&
           !(reg(TRANS_DDI_FUNC_CTL(I915_TRANS_EDP)) & TRANS_DDI_FUNC_ENABLE) &&
           !(reg(DDI_BUF_CTL(0)) & DDI_BUF_CTL_ENABLE) && !i915_model_pipe_on(&model, 0) &&
           !i915_model_panel(&model)->on &&
           !(reg(PP_CONTROL) & (PANEL_POWER_ON | EDP_BLC_ENABLE | EDP_FORCE_VDD)) &&
           !(reg(BLC_PWM_PCH_CTL1) & BLM_PCH_PWM_ENABLE);
}

/** Whether the panel shows a picture: powered, its link trained, the
 * pipe, its route, DDI A's buffer and the plane on, the PWM and the
 * backlight's enable on. */
static int display_lit(void)
{
    struct i915_model_sink *sink = i915_model_sink(&model);

    return (reg(DSPCNTR(I915_PIPE_A)) & DISP_ENABLE) &&
           (reg(TRANSCONF(I915_TRANS_EDP)) & TRANSCONF_ENABLE) &&
           (reg(TRANS_DDI_FUNC_CTL(I915_TRANS_EDP)) & TRANS_DDI_FUNC_ENABLE) &&
           (reg(DDI_BUF_CTL(0)) & DDI_BUF_CTL_ENABLE) && i915_model_pipe_on(&model, 0) &&
           i915_model_panel(&model)->on && sink->trained && sink->normal &&
           (reg(PP_CONTROL) & (PANEL_POWER_ON | EDP_BLC_ENABLE)) ==
               (PANEL_POWER_ON | EDP_BLC_ENABLE) &&
           (reg(BLC_PWM_PCH_CTL1) & BLM_PCH_PWM_ENABLE);
}

/* ---- the DRM requests of a master ---- */

static struct drm_file fa;
static struct drm_device *fa_dev; /* the device `fa` is open on, held */

/** Open the master's file on the driver's device, which it then holds, as
 * a program with the card open does. */
static void master_open(void)
{
    fa_dev = card();
    CHECK(fa_dev != NULL, "a device to open");
    if (!fa_dev) {
        return;
    }
    drm_dev_get(fa_dev);
    file_open(&fa, fa_dev);
    fa_dev->master = &fa;
    fa.is_master = 1;
    fa.atomic = fa.universal_planes = 1;
}

/** Close the master's file: its framebuffers go, and its hold on the
 * device. */
static void master_close(void)
{
    int i;

    if (!fa_dev) {
        return;
    }
    drm_mode_file_release(&fa);
    for (i = 0; i < MAX_HANDLES; i++) {
        if (handles[i].file == &fa) {
            handles[i].obj->refs--;
            handles[i].file = NULL;
        }
    }
    if (fa_dev->master == &fa) {
        fa_dev->master = NULL;
    }
    drm_dev_put(fa_dev);
    fa_dev = NULL;
}

/** Make object `i` of `size` bytes, linear, and give the master a handle
 * to it. */
static uint32_t bo_new(int i, uint64_t size)
{
    uint32_t h = 0;

    k_memset(&bo[i], 0, sizeof(bo[i]));
    k_memset(&bo_vma[i], 0, sizeof(bo_vma[i]));
    bo[i].base.dev = fa_dev;
    bo[i].base.size = size;
    bo[i].base.funcs = &i915_gem_object_funcs;
    bo[i].base.resv = &bo[i].base.resv_own;
    bo[i].base.refs = 1;
    bo[i].tiling = I915_TILING_NONE;
    CHECK(drm_gem_handle_create(&fa, &bo[i].base, &h) == 0, "a handle is made");
    bo[i].base.refs--;
    return h;
}

/** A linear XRGB8888 framebuffer of `w` by `h` on object `i`, its rows
 * 64-byte aligned as a dumb buffer's are; its ID, or 0. */
static uint32_t fb_new(int i, uint32_t w, uint32_t h)
{
    struct drm_mode_fb_cmd2 r;
    uint32_t pitch = (w * 4u + 63u) & ~63u;

    k_memset(&r, 0, sizeof(r));
    r.width = w;
    r.height = h;
    r.pixel_format = DRM_FORMAT_XRGB8888;
    r.handles[0] = bo_new(i, (uint64_t)pitch * h);
    r.pitches[0] = pitch;
    return drm_mode_addfb2_ioctl(fa_dev, &r, &fa) == 0 ? r.fb_id : 0;
}

/** GETCONNECTOR of `connector`, which probes it; its status, and its mode
 * of `w` by `h` at `hz` into `out`, or -1 when it has none. */
static int probe(struct drm_connector *connector, uint32_t w, uint32_t h, uint32_t hz,
                 struct drm_mode_modeinfo *out)
{
    struct drm_mode_get_connector gc;
    struct drm_mode_modeinfo modes[32];
    uint32_t i;

    k_memset(&gc, 0, sizeof(gc));
    gc.connector_id = connector->base.id;
    if (drm_mode_getconnector(fa_dev, &gc, &fa) != 0) {
        return -1;
    }
    gc.modes_ptr = UPTR(modes);
    gc.count_modes = 32;
    gc.count_props = 0;
    gc.count_encoders = 0;
    if (drm_mode_getconnector(fa_dev, &gc, &fa) != 0) {
        return -1;
    }
    for (i = 0; i < gc.count_modes && i < 32; i++) {
        if (modes[i].hdisplay == w && modes[i].vdisplay == h && modes[i].vrefresh == hz) {
            *out = modes[i];
            return (int)gc.connection;
        }
    }
    return -1;
}

/** SETCRTC of pipe A with `mode` and `fb_id` on `connector`. */
static int32_t setcrtc(struct drm_i915_private *i915, struct drm_connector *connector,
                       const struct drm_mode_modeinfo *mode, uint32_t fb_id)
{
    struct drm_mode_crtc c;
    uint32_t conn = connector->base.id;

    k_memset(&c, 0, sizeof(c));
    c.crtc_id = i915->display.crtc[0].base.base.id;
    c.fb_id = fb_id;
    c.mode_valid = 1;
    c.mode = *mode;
    c.set_connectors_ptr = UPTR(&conn);
    c.count_connectors = 1;
    return drm_mode_setcrtc(fa_dev, &c, &fa);
}

/* ---- rows ---- */

static struct drm_gem_object *fw_obj; /* the firmware's framebuffer's object */

/** The driver's device of the registered card, or NULL. */
static struct drm_i915_private *bound(void)
{
    return card() ? to_i915(card()) : NULL;
}

/**
 * The boot: the model as the firmware leaves the chip, the boot's
 * backlight registered, and the driver bound through the test's boundary,
 * which adopts pipe A. /sys then shows the driver's directory with its two
 * files, and the driver on the function.
 */
static void row_boot(void)
{
    struct drm_i915_private *i915;

    CHECK(i915_model_init(&model) == 0, "the model is made");
    clock_base_ns = model_now_ns - (int64_t)model.now_us * 1000;
    model.on_delay = follow_clock;
    i915_model_irq_state(&model)->dma = test_dma;
    model_ops = model.hw.ops;
    chip_hw = model.hw;
    chip_hw.ops = &chip_ops;
    CHECK(opregion_load(), "the OpRegion is read");
    /* The firmware's 937 levels are 200 Hz in steps of 128 clocks. */
    i915_model_poke(&model, SOUTH_CHICKEN2, reg(SOUTH_CHICKEN2) | LPT_PWM_GRANULARITY);
    boot_screen.addr = LENOVO_GPU_BAR2;
    boot_screen.pitch = LENOVO_FW_FB_STRIDE;
    boot_screen.width = LENOVO_FW_FB_WIDTH;
    boot_screen.height = LENOVO_FW_FB_HEIGHT;
    boot_screen.bpp = 32;
    boot_screen.type = BOOT_FB_RGB;
    CHECK(backlight_register("intel_backlight", 937, &boot_ops, NULL) == 0,
          "the boot registers intel_backlight");
    CHECK(sysfs_add_attr(sysfs_anchor(SYSFS_PCI_DEVICE, LENOVO_GPU_DEVICE << 3), &attr_override,
                         NULL, 0) > 0,
          "VFIO's driver_override is on the function");
    model_wait = sleep_frames;
    CHECK(i915_driver_bind(&model.pci, &chip_hw) == 0 && (i915 = bound()) != NULL,
          "boot: the driver binds");
    if (!bound()) {
        return;
    }
    i915 = bound();
    CHECK(logged("registered: i915 1.6.0") && logged("rings: rcs0, bcs0, vcs0 and vecs0 started") &&
              logged("display: pipe A adopted: 1366x768 in 1366x768@60") &&
              i915->display.crtc[0].base.state->active && i915->ggtt.nkept == 1,
          "boot: registered, the rings started, pipe A adopted with the firmware's framebuffer");
    fw_obj = i915->display.crtc[0].primary.state->fb->obj[0];
    CHECK(sys_bus_path("drivers", "i915", "bind") > 0 &&
              sys_bus_path("drivers", "i915", "unbind") > 0 && sys_bound(),
          "boot: /sys has the driver's directory, bind and unbind, and the driver on the function");
    CHECK(owner && strcmp(owner, PCI_OWNER_KERNEL) == 0 && msi_on() && vectors_live == 1 &&
              (command() & (PCI_CMD_BUS_MASTER | PCI_CMD_MEMORY_SPACE)) ==
                  (PCI_CMD_BUS_MASTER | PCI_CMD_MEMORY_SPACE),
          "boot: the kernel owns the function, its message, decoding and bus mastering on");
    CHECK(strcmp(console_calls, "r") == 0 && aperture_removals == 1,
          "boot: the console told of the device, the firmware's device removed");
    CHECK(bl_show(BACKLIGHT_LEVELS) == 937 && !boot_has,
          "boot: i915 drives intel_backlight with the firmware's 937 levels");
    CHECK(i915->hw.timeouts == 0 && !timed_out(), "boot: no wait at its bound");
}

/** The files' refusals: `bind` of a function the driver has, and names
 * that are none or no function of the driver's; none reaches the chip. */
static void row_refusals(void)
{
    struct drm_i915_private *i915 = bound();
    uint32_t n = accesses;

    CHECK(driver_write("bind", CHIP "\n") == -EBUSY, "refusals: bind of a bound function: EBUSY");
    CHECK(driver_write("unbind", "0000:00:03.0\n") == -ENODEV &&
              driver_write("unbind", "00:02.0\n") == -ENODEV &&
              driver_write("unbind", "0000:00:02.8\n") == -ENODEV &&
              driver_write("unbind", CHIP "x\n") == -ENODEV &&
              driver_write("unbind", "") == -ENODEV,
          "refusals: unbind of another function or of no name: ENODEV");
    CHECK(driver_write("bind", "0000:00:1f.0\n") == -ENODEV &&
              driver_write("bind", "i915\n") == -ENODEV,
          "refusals: bind of a function that is not there or of no name: ENODEV");
    CHECK(accesses == n && bound() == i915 && sys_bound() && resets == 0,
          "refusals: the chip untouched, the driver still bound");
}

/** A release while a program sleeps with a window of the aperture: EBUSY,
 * with the display as it was and the console told to look again. */
static void row_held(void)
{
    struct drm_i915_private *i915 = bound();

    console_clear();
    marks_clear();
    CHECK(display_lit(), "held: the panel lit as the firmware left it");
    i915->gem.aperture.win[0].users = 1;
    CHECK(driver_write("unbind", CHIP "\n") == -EBUSY, "held: unbind: EBUSY");
    i915->gem.aperture.win[0].users = 0;
    CHECK(bound() == i915 && sys_bound() && display_lit() && !mark.blc_off && !mark.gdrst &&
              i915->display.crtc[0].base.state->active && strcmp(console_calls, "h") == 0,
          "held: the driver bound, the panel lit, the GPU not reset, the console told");
}

static uint32_t cycle_ms; /* the panel's power cycle, as the driver had it */

/** Whether the display went down in Linux's order since `marks_clear`: the
 * backlight, the transcoder, DDI A's buffer, then the panel's power; and
 * the GPU was reset after that. */
static int down_in_order(void)
{
    return mark.blc_off && mark.blc_off < mark.trans_off && mark.trans_off < mark.ddi_off &&
           mark.ddi_off < mark.panel_off && mark.panel_off < mark.gdrst;
}

/** Whether the function is as a release leaves it: the display off, the
 * rings stopped and empty, every interrupt masked with the message off and
 * its vector back, bus mastering off, no owner, and /sys without the
 * driver on it, the directory and its files still there. */
static int released(void)
{
    return display_off() && rings_idle() && rings_running_at_unplug == 0 && interrupts_off() &&
           !msi_on() && vectors_live == 0 && !(command() & PCI_CMD_BUS_MASTER) && owner == NULL &&
           !sys_bound() && sys_bus_path("drivers", "i915", "bind") > 0 &&
           sys_bus_path("drivers", "i915", "unbind") > 0 && card() == NULL;
}

/** Whether the record `dev`, which a program still holds, is cut off: its
 * reads all ones, its writes dropped, and none reaching the chip. */
static int cut_off(struct drm_device *dev)
{
    struct i915_hw *hw = &to_i915(dev)->hw;
    uint32_t n = accesses, deier = reg(DEIER), pte = reg(I915_GSM_OFFSET);
    int ok;

    ok = dev->unplugged && i915_read(hw, DEIER) == 0xffffffffu &&
         i915_read(hw, PP_STATUS) == 0xffffffffu && i915_gtt_read(hw, 0) == 0xffffffffu;
    i915_write(hw, DEIER, DE_MASTER_IRQ_CONTROL);
    i915_gtt_write(hw, 0, 0x12345001u);
    return ok && accesses == n && reg(DEIER) == deier && reg(I915_GSM_OFFSET) == pte;
}

/**
 * The release, from the panel as the firmware lit it, with a program that
 * has the card open: everything the comment at the top lists, and the
 * record freed with its pages when the program lets go.
 */
static void row_release(void)
{
    struct drm_i915_private *i915 = bound();
    struct drm_device *held = card();
    uint32_t n;
    int sets;

    cycle_ms = i915->display.dp.pps.power_cycle_ms;
    drm_dev_get(held);
    console_clear();
    marks_clear();
    CHECK(driver_write("unbind", CHIP "\n") == 13, "release: unbind taken");
    CHECK(down_in_order(),
          "release: the backlight, the transcoder, the DDI and the panel's power off in that "
          "order, then the GPU reset");
    CHECK(request_unplugs == 1 && gem_unplugs == 1 &&
              i915_model_reset_state(&model)->resets[0] >= 1 && released(),
          "release: the display off, the rings reset before their pages went, interrupts and the "
          "message off, bus mastering off, no owner, /sys without the driver on the function");
    CHECK(strcmp(console_calls, "gu") == 0,
          "release: the console told its screen has gone, then that the device left");
    CHECK(logged("handoff: " CHIP ": released") && cycle_ms >= 500,
          "release: logged, the panel's power cycle of half a second or more remembered");
    sets = boot_sets;
    n = accesses;
    CHECK(boot_has && bl_show(BACKLIGHT_LEVELS) == 937 &&
              bl_store(BACKLIGHT_BRIGHTNESS, "400") >= 0 && bl_show(BACKLIGHT_BRIGHTNESS) == 400 &&
              boot_sets == sets && accesses == n,
          "release: intel_backlight on a stand-in: a level kept, no register written");
    CHECK(releases == 0 && cut_off(held),
          "release: the record a program holds reaches no register");
    drm_dev_put(held);
    CHECK(releases == 1 && pages_live == 0, "release: the record and every page freed at its end");
    CHECK(!timed_out(), "release: no wait at its bound");
}

/** A bring-up while vfio-pci has the function, or while its
 * driver_override names vfio-pci: refused, with no reset and no access. */
static void row_other_owner(void)
{
    uint32_t n = accesses;

    owner = "vfio-pci";
    CHECK(driver_write("bind", CHIP "\n") == -EBUSY, "owner: bind while vfio-pci has it: EBUSY");
    CHECK(driver_write("unbind", CHIP "\n") == -ENODEV,
          "owner: unbind of what is not bound: ENODEV");
    owner = NULL;
    override = "vfio-pci";
    CHECK(driver_write("bind", CHIP "\n") == -ENODEV,
          "owner: bind while driver_override names vfio-pci: ENODEV");
    override = "";
    CHECK(resets == 0 && accesses == n && probe_maps == 0 && owner == NULL && !sys_bound(),
          "owner: no reset, no access, nothing bound");
}

/* The guest's entries of the GTT: their first index, their count and the
 * entry at index `i`. */
#define GUEST_PTE_FIRST 0u
#define GUEST_PTES      4096u
#define GUEST_PTE(i)    (0xa0000001u | (uint32_t)(i) << 12)

/**
 * The function as a guest's driver leaves it when QEMU is killed: a
 * picture on pipe A from the guest's memory, the panel and the backlight
 * on with a period of the guest's, interrupts enabled, the message and bus
 * mastering on, and the GTT full of the guest's pages.
 */
static void row_guest(void)
{
    uint32_t i;

    for (i = 0; i < GUEST_PTES; i++) {
        i915_model_poke(&model, I915_GSM_OFFSET + (GUEST_PTE_FIRST + i) * 4u, GUEST_PTE(i));
    }
    i915_model_poke(&model, DSPCNTR(I915_PIPE_A), DISP_ENABLE | DISP_FORMAT_BGRX888);
    i915_model_poke(&model, DSPSURF(I915_PIPE_A), 0x00400000u);
    i915_model_poke(&model, TRANSCONF(I915_TRANS_EDP), TRANSCONF_ENABLE);
    i915_model_poke(&model, DDI_BUF_CTL(0), reg(DDI_BUF_CTL(0)) | DDI_BUF_CTL_ENABLE);
    i915_model_poke(&model, PP_CONTROL, PANEL_POWER_ON | EDP_BLC_ENABLE);
    i915_model_poke(&model, BLC_PWM_PCH_CTL1, BLM_PCH_PWM_ENABLE);
    i915_model_poke(&model, BLC_PWM_PCH_CTL2, 0x12340234u);
    i915_model_poke(&model, DEIMR, 0);
    i915_model_poke(&model, DEIER, DE_MASTER_IRQ_CONTROL | 0xffu);
    i915_model_poke(&model, GTIMR, 0);
    i915_model_poke(&model, GTIER, 0xffu);
    i915_model_panel(&model)->on = 1;
    i915_model_sink(&model)->trained = 1;
    model.config[LENOVO_GPU_CAP_MSI + 2] |= 1u;
    model.config[PCI_COMMAND] |= PCI_CMD_BUS_MASTER;
    CHECK(!display_off() && !interrupts_off() && msi_on(),
          "guest: the function as a guest left it");
}

/** A bring-up whose reset fails: the reset's error, with no register read
 * or written and nothing bound. */
static void row_reset_fails(void)
{
    uint32_t n = accesses;

    log_text[0] = 0;
    reset_error = -ETIMEDOUT;
    CHECK(driver_write("bind", CHIP "\n") == -ETIMEDOUT, "reset fails: bind: the reset's error");
    reset_error = 0;
    CHECK(resets == 1 && accesses == n && probe_maps == 0 && owner == NULL && !sys_bound() &&
              bound() == NULL && (reg(DEIER) & DE_MASTER_IRQ_CONTROL) &&
              logged("handoff: " CHIP ": not reset: error"),
          "reset fails: no access, no owner, nothing bound, logged");
}

/** A bring-up whose bind fails at the registration, after the reset: the
 * bind's error, with everything it took given back and the function left
 * without an owner, for the next bind to take. */
static void row_bind_fails(void)
{
    int r = releases;

    log_text[0] = 0;
    register_error = -ENOSPC;
    CHECK(driver_write("bind", CHIP "\n") == -ENOSPC, "bind fails: bind: the registration's error");
    CHECK(resets == 2 && owner == NULL && !(command() & PCI_CMD_BUS_MASTER) && !msi_on() &&
              vectors_live == 0 && interrupts_off() && !sys_bound() && bound() == NULL &&
              releases == r + 1 && pages_live == 0 && logged("handoff: " CHIP ": not bound: error"),
          "bind fails: no owner, bus mastering, the message and interrupts off, the record freed");
}

/**
 * The bring-up: the function reset once, with no owner, before any access;
 * the first access the reset's 100 ms and the panel's power cycle later;
 * the driver bound with nothing adopted and none of the guest's entries in
 * the GTT; and the kernel, /sys, the console and the backlight as after
 * the boot's bind, with the PWM's period from the VBT.
 */
static void row_bring_up(const char *name)
{
    struct drm_i915_private *i915;
    int before = resets, maps = probe_maps;
    uint32_t i, left = 0;

    log_text[0] = 0;
    console_clear();
    marks_clear();
    CHECK(driver_write("bind", CHIP "\n") == 13, name);
    i915 = bound();
    CHECK(i915 != NULL, "bring-up: a device registered");
    if (!i915) {
        return;
    }
    CHECK(resets == before + 1 && reset_owner_bad == 0 && probe_maps == maps + 1,
          "bring-up: the function reset once, while it had no owner, and mapped once");
    CHECK(first_access_seen && first_access_us >= reset_at_us + (100u + cycle_ms) * 1000u,
          "bring-up: no access until the reset's 100 ms and the panel's power cycle had passed");
    CHECK(logged("handoff: " CHIP ": bound from a reset") && logged("registered: i915 1.6.0") &&
              logged("rings: rcs0, bcs0, vcs0 and vecs0 started"),
          "bring-up: registered, the rings started");
    for (i = 0; i < GUEST_PTES; i++) {
        left += reg(I915_GSM_OFFSET + (GUEST_PTE_FIRST + i) * 4u) == GUEST_PTE(i);
    }
    CHECK(
        !logged("adopted") && i915->ggtt.nkept == 0 && !i915->display.crtc[0].base.state->active &&
            display_off() && left == 0 &&
            reg(I915_GSM_OFFSET + 4u * (GUEST_PTES - 1u)) == i915->ggtt.scratch_pte,
        "bring-up: nothing adopted, the display off, the guest's GTT entries on the scratch page");
    CHECK(owner && strcmp(owner, PCI_OWNER_KERNEL) == 0 && msi_on() && vectors_live == 1 &&
              (command() & (PCI_CMD_BUS_MASTER | PCI_CMD_MEMORY_SPACE)) ==
                  (PCI_CMD_BUS_MASTER | PCI_CMD_MEMORY_SPACE) &&
              (reg(DEIER) & DE_MASTER_IRQ_CONTROL) && !i915->gt.wedged,
          "bring-up: the kernel owns the function, its message and interrupts on");
    CHECK(sys_bound() && card()->index == 0 && strcmp(console_calls, "r") == 0,
          "bring-up: /sys shows the driver on the function, card0 again, the console told");
    CHECK(bl_show(BACKLIGHT_LEVELS) == 938,
          "bring-up: i915 drives intel_backlight with the VBT's period, 938 levels");
    CHECK(i915->hw.timeouts == 0 && !timed_out(), "bring-up: no wait at its bound");
}

/**
 * A full mode set of the panel by a master's SETCRTC of its own
 * framebuffer on object `k`: the panel powered no sooner than its power
 * cycle after the reset, the link trained, the pipe, the plane and the
 * backlight on, and ten frames counted.
 */
static void row_mode_set(int k)
{
    struct drm_i915_private *i915 = bound();
    struct drm_mode_modeinfo mode;
    uint64_t count;
    uint32_t fb;
    int32_t err;
    int i;

    master_open();
    if (!i915 || !fa_dev) {
        return;
    }
    fb = fb_new(k, 1366, 768);
    CHECK(fb != 0 && probe(&i915->display.connector, 1366, 768, 60, &mode) == 1,
          "mode set: the panel connected, with its 1366x768 mode at 60 Hz");
    marks_clear();
    count = i915->display.crtc[0].base.vblank.count;
    err = fb ? setcrtc(i915, &i915->display.connector, &mode, fb) : -1;
    if (err != 0) {
        printf("i915-handoff-test: mode set: SETCRTC: error %d\n", (int)err);
    }
    for (i = 0; i < 10; i++) {
        frame();
    }
    CHECK(
        err == 0 && display_lit() && i915_model_power(&model)->on &&
            i915->display.crtc[0].base.state->active,
        "mode set: the panel powered, the link trained, the pipe, the plane and the backlight on");
    CHECK(mark.panel_on && mark.panel_on_us >= reset_at_us + cycle_ms * 1000u,
          "mode set: the panel's power on no sooner than its power cycle after the reset");
    CHECK((reg(DSPSURF(I915_PIPE_A)) & ~0xfffu) == BO_GGTT(k) && bo_vma[k].pin_count == 1,
          "mode set: the plane shows the master's framebuffer");
    CHECK(i915->display.crtc[0].base.vblank.count >= count + 10, "mode set: ten frames counted");
    CHECK(i915->hw.timeouts == 0 && !timed_out(), "mode set: no wait at its bound");
}

/**
 * The release a second time, from a panel that a master lit and still
 * holds: the same order and the same end, the master's scanout unpinned,
 * its record cut off, and freed when the master closes the card.
 */
static void row_release_lit(int k)
{
    struct drm_device *held = fa_dev;
    int r = releases, unplugs = request_unplugs;

    console_clear();
    marks_clear();
    CHECK(driver_write("unbind", CHIP "\n") == 13, "release, lit: unbind taken");
    CHECK(down_in_order() && request_unplugs == unplugs + 1 && released(),
          "release, lit: the display down in order, the GPU reset, the function let go");
    CHECK(strcmp(console_calls, "u") == 0 && bo_vma[k].pin_count == 0,
          "release, lit: the console told the device left, the master's scanout unpinned");
    CHECK(releases == r && held && cut_off(held),
          "release, lit: the master's record reaches no register");
    master_close();
    CHECK(releases == r + 1 && pages_live == 0,
          "release, lit: the record and every page freed when the master closed");
    CHECK(!timed_out(), "release, lit: no wait at its bound");
}

/**
 * A GPU that will not reset: the release fails with the reset's error and
 * the driver stays bound, the display off, the GPU wedged and the console
 * told to look again. Once the reset works, the release does.
 */
static void row_stuck(int k)
{
    struct drm_i915_private *i915 = bound();
    struct drm_device *held = fa_dev;
    int r = releases;

    console_clear();
    i915_model_reset_state(&model)->stuck = GEN6_GRDOM_FULL;
    CHECK(driver_write("unbind", CHIP "\n") == -ETIMEDOUT, "stuck: unbind: ETIMEDOUT");
    i915_model_reset_state(&model)->stuck = 0;
    CHECK(bound() == i915 && sys_bound() && owner && msi_on() && vectors_live == 1 &&
              (command() & PCI_CMD_BUS_MASTER) && i915 && i915->gt.wedged && display_off() &&
              strcmp(console_calls, "h") == 0 && logged("handoff: " CHIP ": not released: error"),
          "stuck: the driver bound, the GPU wedged, the display off, the console told");
    log_text[0] = 0;
    console_clear();
    CHECK(driver_write("unbind", CHIP "\n") == 13 && released() && strcmp(console_calls, "u") == 0,
          "stuck: released once the GPU resets");
    CHECK(bo_vma[k].pin_count == 0 && releases == r && held && cut_off(held),
          "stuck: the master's scanout unpinned, its record cut off");
    master_close();
    CHECK(releases == r + 1 && pages_live == 0, "stuck: the record and every page freed");
}

/* ---- RPS's waitboost and interactive mode, which the driver calls ---- */

/* The requests' fences, i915_gem.c's, which no flip here waits on. */
const struct dma_fence_ops i915_fence_ops = {0, 0, 0, 0, 0, 0};

/** Whether the chip has begun `rq`; no request runs here, so only once
 * it has signalled. */
int i915_request_started(const struct i915_request *rq)
{
    return rq->fence.base.signaled;
}

int main(void)
{
    int base;

    model_start("i915-handoff-test");
    base = allocations;
    row_boot();
    if (bound()) {
        row_refusals();
        row_held();
        row_release();
        row_other_owner();
        row_guest();
        row_reset_fails();
        row_bind_fails();
        row_bring_up("bring-up: bind taken");
        row_mode_set(0);
        row_release_lit(0);
        override = "i915";
        row_bring_up("bring-up, again: bind taken with driver_override naming i915");
        override = "";
        row_mode_set(1);
        row_stuck(1);
    }
    if (failures != 0) {
        show_log();
    }
    if (fw_obj && fw_obj->refs == 0) {
        fw_obj->funcs->free(fw_obj);
    }
    CHECK(allocations == base && pages_live == 0 && vectors_live == 0,
          "every allocation, page and vector given back");
    i915_model_free(&model);
    if (failures == 0) {
        printf("i915-handoff-test: ok\n");
    }
    return failures != 0;
}
