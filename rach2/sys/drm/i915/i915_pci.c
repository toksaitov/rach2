/*
 * i915_pci.c: the Intel graphics driver's binding to Haswell GT2 chips,
 * the HD 4400 among them: their PCI ids, the probe, and the DRM device.
 */

#include "i915.h"
#include "i915_reg.h"

#include "../../cmdline.h"
#include "../../irq.h"
#include "../../klog.h"
#include "../../lib/kerrno.h"
#include "../../lib/kstring.h"
#include "../../mm/kheap.h"
#include "../../mm/paging.h"

#define PCI_VENDOR_INTEL 0x8086

/* The ids, the class and revision, and the board's ids in configuration
 * space. */
#define PCI_ID_DWORD            0x00
#define PCI_CLASS_REVISION      0x08
#define PCI_SUBSYSTEM_VENDOR_ID 0x2c
#define PCI_SUBSYSTEM_ID        0x2e

/* Linux 6.12's hsw_gt2_info (i915_pci.c), which every id below shares. */
static const struct i915_platform hsw_gt2 = {
    "HASWELL", 7, 50,
    7,         2, I915_ENGINE_RCS0 | I915_ENGINE_BCS0 | I915_ENGINE_VCS0 | I915_ENGINE_VECS0,
    1, /* has_llc */
    1, /* has_l3_dpf */
    1, /* has_rc6 */
    0, /* has_rc6p */
    1, /* has_reset_engine */
    31,        40};

/* INTEL_HSW_GT2_IDS of include/drm/intel/i915_pciids.h, in its order:
 * the ULT parts, the ULX part, then the desktop, mobile, server and
 * reserved parts of the HSW, SDV and CRW lines. */
const struct i915_id i915_ids[] = {
    {0x0A12, 1, 0, &hsw_gt2}, {0x0A16, 1, 0, &hsw_gt2}, {0x0A1A, 1, 0, &hsw_gt2},
    {0x0A1B, 1, 0, &hsw_gt2}, {0x0A1E, 1, 1, &hsw_gt2}, {0x0412, 0, 0, &hsw_gt2},
    {0x0416, 0, 0, &hsw_gt2}, {0x041A, 0, 0, &hsw_gt2}, {0x041B, 0, 0, &hsw_gt2},
    {0x041E, 0, 0, &hsw_gt2}, {0x0C12, 0, 0, &hsw_gt2}, {0x0C16, 0, 0, &hsw_gt2},
    {0x0C1A, 0, 0, &hsw_gt2}, {0x0C1B, 0, 0, &hsw_gt2}, {0x0C1E, 0, 0, &hsw_gt2},
    {0x0D12, 0, 0, &hsw_gt2}, {0x0D16, 0, 0, &hsw_gt2}, {0x0D1A, 0, 0, &hsw_gt2},
    {0x0D1B, 0, 0, &hsw_gt2}, {0x0D1E, 0, 0, &hsw_gt2}};
const int i915_nids = (int)(sizeof(i915_ids) / sizeof(i915_ids[0]));

/* The devices bound, for the interrupt handler to find by vector, and the
 * spinlock over the table and each device's `irq.inflight`. */
static struct drm_i915_private *i915_devices[4];
static struct spinlock i915_devices_lock = SPINLOCK_INIT_NAMED("i915_devices_lock");

static void i915_unplug(struct drm_device *dev);
static void i915_release(struct drm_device *dev);

/* What the driver says of itself: Linux's i915, with its buffer ioctls. */
static const struct drm_driver i915_drm_driver = {
    DRM_IDENT_I915,       i915_driver_open,      i915_driver_postclose,
    i915_gem_dumb_create, i915_gem_prime_import, i915_unplug,
    i915_ioctls,          I915_NUM_IOCTLS,       i915_release};

const struct i915_id *i915_id_lookup(uint16_t device_id)
{
    int i;

    for (i = 0; i < i915_nids; i++) {
        if (i915_ids[i].device_id == device_id) {
            return &i915_ids[i];
        }
    }
    return 0;
}

/**
 * The device's message: counted, then handled by i915_irq.c, which hands
 * each source to the handler its owner registered. It runs without the big
 * lock (ISR_UNLOCKED), taking only the driver's spinlocks and the DRM
 * core's, so a device is found and counted as running under the table's
 * lock, which `i915_irq_sync` waits on.
 */
static void i915_irq_handler(isr_params_t *params)
{
    struct drm_i915_private *i915;
    uint32_t flags;
    unsigned i;

    for (i = 0; i < sizeof(i915_devices) / sizeof(i915_devices[0]); i++) {
        flags = spin_lock_irqsave(&i915_devices_lock);
        i915 = i915_devices[i];
        if (!i915 || i915->msi_vector != (int)params->vector) {
            spin_unlock_irqrestore(&i915_devices_lock, flags);
            continue;
        }
        i915->irq.inflight++;
        i915->irqs++;
        spin_unlock_irqrestore(&i915_devices_lock, flags);
        i915_irq_handle(i915);
        flags = spin_lock_irqsave(&i915_devices_lock);
        i915->irq.inflight--;
        spin_unlock_irqrestore(&i915_devices_lock, flags);
    }
}

void i915_irq_sync(struct drm_i915_private *i915)
{
    uint32_t flags, n;

    do {
        flags = spin_lock_irqsave(&i915_devices_lock);
        n = i915->irq.inflight;
        spin_unlock_irqrestore(&i915_devices_lock, flags);
    } while (n != 0);
}

/**
 * Take a vector for the device's message and enable the message on it,
 * where the machine has messages. A machine without them leaves the device
 * with none, which nothing needs yet; that is logged, and `msi_vector`
 * stays -1.
 */
static void i915_msi_start(struct drm_i915_private *i915)
{
    int vector;

    if (!isr_msi_ready() || (vector = isr_alloc_vector()) < 0) {
        klog(KLOG_WARN, "i915", "interrupts: none: the machine has no message vector");
        return;
    }
    i915->msi_vector = vector;
    register_isr_handler((uint8_t)vector, i915_irq_handler, "i915");
    isr_register_flags((uint8_t)vector, i915_irq_handler, ISR_UNLOCKED);
    if (pci_msi_enable(&i915->pci, (uint8_t)vector) != 0) {
        isr_free_vector(vector);
        i915->msi_vector = -1;
        klog(KLOG_WARN, "i915", "interrupts: none: the message would not enable");
        return;
    }
    klog(KLOG_INFO, "i915", "interrupts: messages on vector %d", vector);
}

/** Stop the device's message and give its vector back. */
static void i915_msi_stop(struct drm_i915_private *i915)
{
    if (i915->msi_vector < 0) {
        return;
    }
    pci_msi_disable(&i915->pci);
    isr_free_vector(i915->msi_vector);
    i915->msi_vector = -1;
}

/** Add a bound device to the handler's table; 0, or -ENOSPC when full. */
static int32_t i915_devices_add(struct drm_i915_private *i915)
{
    uint32_t flags = spin_lock_irqsave(&i915_devices_lock);
    unsigned i;

    for (i = 0; i < sizeof(i915_devices) / sizeof(i915_devices[0]); i++) {
        if (!i915_devices[i]) {
            i915_devices[i] = i915;
            spin_unlock_irqrestore(&i915_devices_lock, flags);
            return 0;
        }
    }
    spin_unlock_irqrestore(&i915_devices_lock, flags);
    return -ENOSPC;
}

/** Take a device out of the handler's table, and wait for a handler of
 * it that another processor runs, so that nothing torn down after reaches
 * it. */
static void i915_devices_remove(struct drm_i915_private *i915)
{
    uint32_t flags = spin_lock_irqsave(&i915_devices_lock);
    unsigned i;

    for (i = 0; i < sizeof(i915_devices) / sizeof(i915_devices[0]); i++) {
        if (i915_devices[i] == i915) {
            i915_devices[i] = 0;
        }
    }
    spin_unlock_irqrestore(&i915_devices_lock, flags);
    i915_irq_sync(i915);
}

/** Give back the MMIO backend's mapping of the GTT's entries (`gsm`);
 * nothing for another backend, whose entries are its own memory. */
static void i915_gsm_unmap(struct i915_hw *hw)
{
    if (i915_hw_is_mmio(hw) && hw->gsm) {
        mmio_unmap((uintptr_t)hw->gsm, hw->gtt_entries * 4u);
        hw->gsm = 0;
    }
}

void i915_pci_unmap(struct i915_hw *hw)
{
    i915_gsm_unmap(hw);
    if (i915_hw_is_mmio(hw) && hw->priv) {
        mmio_unmap((uintptr_t)hw->priv, hw->size);
        hw->priv = 0;
    }
    i915_hw_gone(hw);
}

/**
 * The DRM core's unplug hook: its requests fail and retire, RPS and RC6
 * turned off, the device stops interrupting, its message and vector are
 * given back, its rings stop and give their pages back, and programs' maps
 * of its buffers are revoked. /sys no longer shows the driver on the
 * function. The record stays until its last reference.
 */
static void i915_unplug(struct drm_device *dev)
{
    struct drm_i915_private *i915 = to_i915(dev);
    int taken = i915_gem_lock(i915);

    i915_handoff_unbound(i915);
    i915_requests_unplug(i915);
    i915_gt_pm_disable(i915);
    intel_display_fini(i915);
    if (i915->attached && i915->hw.ops->detach) {
        i915->hw.ops->detach(&i915->hw);
    }
    i915_irq_reset(i915);
    i915_msi_stop(i915);
    i915_devices_remove(i915);
    i915_gt_fini(i915);
    i915_gem_unplug(i915);
    i915_gem_unlock(i915, taken);
}

/** The DRM core's release hook: take the shrinker off, free the display's
 * objects, unmap the aperture's windows, give the scratch page back, give
 * the backend and its mappings back, and free the record. */
static void i915_release(struct drm_device *dev)
{
    struct drm_i915_private *i915 = to_i915(dev);

    i915_gem_shrinker_unregister(i915);
    if (i915->display.ready) {
        drm_mode_config_cleanup(dev);
    }
    i915_aperture_fini(&i915->gem.aperture);
    i915_ggtt_fini(i915);
    i915_gsm_unmap(&i915->hw);
    intel_bios_fini(&i915->vbt);
    intel_opregion_fini(&i915->opregion);
    if (i915->attached) {
        if (i915->hw.ops->release) {
            i915->hw.ops->release(&i915->hw);
        }
        i915_pci_unmap(&i915->hw);
    }
    kfree(i915);
}

/**
 * Read the identity of the device from the backend's configuration space,
 * where it answers for one, into `ident`, and have /sys report it for the
 * function in place of the function's own. The chip's own function needs
 * nothing: /sys reads its configuration space.
 */
static void i915_ident(struct drm_i915_private *i915)
{
    struct sysfs_pci_ident *id = &i915->ident;
    uint32_t ids, cr;

    if (!i915->hw.ops->config_read32) {
        return;
    }
    ids = i915_hw_config32(&i915->hw, &i915->pci, PCI_ID_DWORD);
    cr = i915_hw_config32(&i915->hw, &i915->pci, PCI_CLASS_REVISION);
    id->vendor = (uint16_t)ids;
    id->device = (uint16_t)(ids >> 16);
    id->subsystem_vendor = i915->subsystem_vendor;
    id->subsystem_device = i915->subsystem_device;
    id->class = cr >> 8;
    id->revision = (uint8_t)cr;
    id->boot_vga = 0;
    i915->drm.pci_ident = id;
}

/* The option ROM's shadow, where the firmware leaves the ROM of the
 * device that drove the screen at boot (Linux's pci_map_rom with
 * IORESOURCE_ROM_SHADOW). */
#define I915_ROM_SHADOW      0xc0000u
#define I915_ROM_SHADOW_SIZE 0x20000u
#define PCI_CLASS_DISPLAY    0x03
#define PCI_COMMAND_IO       0x1
#define PCI_COMMAND_MEMORY   0x2

void i915_bios_setup(struct drm_i915_private *i915, uint16_t command)
{
    const pci_device_t *p = &i915->pci;
    uint32_t asls, size = 0;
    uint8_t *vbt = NULL;
    uintptr_t rom;
    int32_t err;

    asls = pci_read_config32(p->bus, p->device, p->function, ASLS);
    err = intel_opregion_setup(&i915->opregion, asls);
    if (err < 0 && err != -ENODEV) {
        klog(KLOG_WARN, "i915", "opregion: at 0x%08x not used: error %d", (unsigned)asls, (int)err);
    }
    if (i915->opregion.vbt) {
        err = intel_bios_init(&i915->vbt, i915->opregion.vbt, i915->opregion.vbt_size,
                              i915->opregion.rvda ? "OpRegion (RVDA)" : "OpRegion (Mailbox #4)");
    } else {
        /* The x86 boot display's ROM is the shadow, which it decoded with
         * I/O and memory on, as Linux's pci_fixup_video judges. */
        if (p->class_code == PCI_CLASS_DISPLAY && p->subclass == 0 &&
            (command & (PCI_COMMAND_IO | PCI_COMMAND_MEMORY)) ==
                (PCI_COMMAND_IO | PCI_COMMAND_MEMORY)) {
            rom = mmio_map(I915_ROM_SHADOW, I915_ROM_SHADOW_SIZE, PTE_NOCACHE);
            if (rom) {
                vbt = intel_bios_rom_vbt((const uint8_t *)rom, I915_ROM_SHADOW_SIZE, &size);
                mmio_unmap(rom, I915_ROM_SHADOW_SIZE);
            }
        }
        err = intel_bios_init(&i915->vbt, vbt, size, vbt ? "PCI ROM" : NULL);
        kfree(vbt);
    }
    if (err < 0) {
        klog(KLOG_ERR, "i915", "vbt: decoded in part: error %d", (int)err);
    }
}

/**
 * Bring up the parts later ones build on, in Linux's order: forcewake and
 * the GT FIFO, the display power well, stolen memory and the global GTT.
 * A well that does not come on and stolen memory that fails its checks are
 * logged and the bind goes on, as on Linux; 0, or the GTT's error.
 */
static int32_t i915_base_init(struct drm_i915_private *i915)
{
    int32_t err;

    i915_uncore_init(i915);
    err = i915_power_init(i915);
    if (err < 0) {
        klog(KLOG_WARN, "i915", "power: the display well did not come on: error %d", (int)err);
    }
    i915_stolen_init(i915);
    return i915_ggtt_init(i915);
}

/**
 * Take over the display of a bound device: remove the firmware's
 * framebuffer device over the aperture, as Linux's i915_driver_probe does
 * before it touches the display, then adopt the pipes. A device whose
 * display could not be made keeps its GEM and loses mode setting, as Linux's
 * i915 on a device without display. The fake chip has no aperture behind
 * it, so its firmware device stays.
 *
 * Under `rach2.i915.disable_display`, Linux's i915.disable_display, the
 * device is one without display from the start, and the display, the
 * backlight and the firmware's framebuffer device are left as the firmware
 * left them, unlike Linux, which removes that device whatever the option
 * says: the console keeps drawing on it, so that a trial of the GPU leaves
 * the screen as it was.
 */
static void i915_display_bringup(struct drm_i915_private *i915)
{
    int32_t err;

    if (kcmdline_has("rach2.i915.disable_display")) {
        klog(KLOG_INFO, "i915",
             "display: disabled by rach2.i915.disable_display; the firmware's framebuffer stays");
        i915->drm.features &= ~(DRIVER_MODESET | DRIVER_ATOMIC);
        return;
    }
    if (i915->gmadr_phys != 0 && i915->ggtt.mappable_end != 0) {
        (void)drm_aperture_remove_conflicting(i915->gmadr_phys, i915->ggtt.mappable_end, "i915");
    }
    err = intel_display_init(i915);
    if (err != 0) {
        klog(KLOG_ERR, "i915", "display: not made: error %d", (int)err);
        intel_display_fini(i915);
        i915->drm.features &= ~(DRIVER_MODESET | DRIVER_ATOMIC);
    }
}

/** What a registered device adds outside the DRM core: its files of /sys
 * and debugfs, the shrinker, and the driver's links on the function. */
static void i915_registered(struct drm_i915_private *i915)
{
    i915_sysfs_register(i915);
    i915_gem_shrinker_register(i915);
    i915_debugfs_register(i915);
    i915_handoff_bound(i915);
}

/**
 * Undo a bind that failed before its registration, in reverse, as Linux's
 * i915_driver_probe unwinds: the aperture's windows, the GTT, the display
 * power well's reference, the GTT's mapping, the VBT and the OpRegion; put
 * the command word back to `command` and free the record. The registers'
 * mapping is the caller's.
 */
static void bind_unwind(struct drm_i915_private *i915, uint16_t command)
{
    const pci_device_t *pdev = &i915->pci;

    i915_aperture_fini(&i915->gem.aperture);
    i915_ggtt_fini(i915);
    if (i915->power.count) {
        i915_power_put(i915);
    }
    i915_gsm_unmap(&i915->hw);
    intel_bios_fini(&i915->vbt);
    intel_opregion_fini(&i915->opregion);
    pci_write_config16(pdev->bus, pdev->device, pdev->function, PCI_COMMAND, command);
    kfree(i915);
}

/** Make the locks of a new device record, each of one class for every
 * device (lib/spinlock.h, proc/mutex.h). */
static void i915_locks_init(struct drm_i915_private *i915)
{
    unsigned e;

    mutex_init(&i915->gem.lock, "i915_gem");
    spin_lock_init(&i915->gem.userptr_lock, "i915_userptr");
    spin_lock_init(&i915->uncore.lock, "i915_uncore");
    spin_lock_init(&i915->irq.lock, "i915_irq");
    spin_lock_init(&i915->rps.lock, "i915_rps");
    for (e = 0; e < I915_NUM_ENGINES; e++) {
        spin_lock_init(&i915->gt.engine[e].lock, "i915_engine");
    }
}

int32_t i915_driver_bind(const pci_device_t *pdev, const struct i915_hw *hw)
{
    const struct i915_id *id;
    struct drm_i915_private *i915;
    uint32_t ids = (uint32_t)pdev->device_id << 16 | pdev->vendor_id;
    uint8_t revision = pdev->revision;
    uint16_t command;
    int32_t err;

    if (hw->ops->config_read32) {
        struct i915_hw backend = *hw;

        ids = i915_hw_config32(&backend, pdev, PCI_ID_DWORD);
        revision = (uint8_t)i915_hw_config32(&backend, pdev, PCI_CLASS_REVISION);
    }
    id = i915_id_lookup((uint16_t)(ids >> 16));
    if (!id || (uint16_t)ids != PCI_VENDOR_INTEL) {
        return -ENODEV;
    }
    i915 = kzalloc(sizeof(*i915));
    if (!i915) {
        return -ENOMEM;
    }
    i915_locks_init(i915);
    i915->pci = *pdev;
    i915->hw = *hw;
    i915->id = id;
    i915->revision = revision;
    i915->msi_vector = -1;
    i915->mmio_phys = pci_bar_address64(pdev, I915_GTTMMADR_BAR);
    i915->gmadr_phys = pci_bar_address64(pdev, I915_GMADR_BAR);
    i915->subsystem_vendor = i915_hw_config16(&i915->hw, pdev, PCI_SUBSYSTEM_VENDOR_ID);
    i915->subsystem_device = i915_hw_config16(&i915->hw, pdev, PCI_SUBSYSTEM_ID);
    i915->gmch_ctrl = i915_hw_config16(&i915->hw, pdev, SNB_GMCH_CTRL);
    i915->bsm = i915_hw_config32(&i915->hw, pdev, INTEL_BSM);

    command = pci_read_config16(pdev->bus, pdev->device, pdev->function, PCI_COMMAND);
    pci_enable_memory_space(&i915->pci);
    pci_enable_bus_master(&i915->pci);
    i915_irq_reset(i915);
    err = i915_base_init(i915);
    if (err == 0) {
        i915_bios_setup(i915, command);
        i915_aperture_init(&i915->gem.aperture, i915->gmadr_phys, i915->ggtt.mappable_end);
        i915_gem_init_hw(i915);
        err = i915_devices_add(i915);
    }
    if (err < 0) {
        bind_unwind(i915, command);
        return err;
    }
    i915_msi_start(i915);
    i915_irq_install(i915);
    i915_rc6_init(i915);
    i915_rps_init(i915);
    if (i915_gt_init(i915) < 0) {
        klog(KLOG_ERR, "i915", "rings: not started; the GPU takes no work");
    } else {
        (void)i915_firstlight(i915);
    }
    i915_gt_pm_enable(i915);

    klog(KLOG_INFO, "i915",
         "found: %02x:%02x.%x: haswell%s (device id %04x), graphics 7.5, display version "
         "7.00, gt%u, revision %02x",
         (unsigned)pdev->bus, (unsigned)pdev->device, (unsigned)pdev->function,
         id->ulx   ? "/ulx"
         : id->ult ? "/ult"
                   : "",
         (unsigned)id->device_id, (unsigned)id->platform->gt, (unsigned)i915->revision);

    drm_dev_init(&i915->drm, &i915_drm_driver);
    i915->drm.pci = &i915->pci;
    i915->drm.driver_priv = i915;
    i915_ident(i915);
    i915_display_bringup(i915);
    err = drm_dev_register(&i915->drm);
    if (err < 0) {
        klog(KLOG_ERR, "i915", "register: error %d", (int)err);
        intel_display_fini(i915);
        i915_gt_pm_disable(i915);
        i915_irq_reset(i915);
        i915_msi_stop(i915);
        i915_devices_remove(i915);
        i915_gt_fini(i915);
        pci_write_config16(pdev->bus, pdev->device, pdev->function, PCI_COMMAND, command);
        drm_dev_put(&i915->drm);
        return err;
    }
    klog(KLOG_INFO, "i915", "registered: i915 1.6.0 for %s on card %d", i915->drm.unique,
         i915->drm.index);
    i915_registered(i915);
    i915->attached = 1;
    if (i915->hw.ops->attach) {
        i915->hw.ops->attach(&i915->hw, i915);
    }
    return 0;
}

int32_t i915_pci_probe(const pci_device_t *pdev)
{
    struct i915_hw hw;
    uint64_t mmio;
    int32_t err;

    if (pdev->function != 0) {
        return -ENODEV;
    }
    mmio = pci_bar_address64(pdev, I915_GTTMMADR_BAR);
    if (mmio == 0) {
        klog(KLOG_ERR, "i915", "%02x:%02x.%x: BAR 0 is not set", (unsigned)pdev->bus,
             (unsigned)pdev->device, (unsigned)pdev->function);
        return -ENXIO;
    }
    err = i915_hw_mmio_init(&hw, mmio, I915_MMIO_SIZE);
    if (err < 0) {
        klog(KLOG_ERR, "i915", "%02x:%02x.%x: registers not mapped: error %d", (unsigned)pdev->bus,
             (unsigned)pdev->device, (unsigned)pdev->function, (int)err);
        return err;
    }
    err = i915_driver_bind(pdev, &hw);
    if (err < 0) {
        i915_pci_unmap(&hw);
    }
    return err;
}

/** Nothing binds, the fake device included, when `i915.modeset`, Linux's
 * int module parameter of -1 (drivers/gpu/drm/i915/i915_params.h, line 49),
 * is 0, or is -1 under `nomodeset`, as i915_check_nomodeset keeps the
 * whole module from loading (drivers/gpu/drm/i915/i915_module.c, lines 22
 * to 50). */
int32_t i915_probe(void)
{
    pci_device_t pdev;
    int32_t err, bound = 0;
    int i, nth, modeset = kcmdline_param_int("i915.modeset", -1);

    if (modeset == 0 || (modeset == -1 && kcmdline_setup("nomodeset"))) {
        klog(KLOG_INFO, "i915", "KMS disabled");
        return -ENODEV;
    }
    for (i = 0; i < i915_nids; i++) {
        for (nth = 0; pci_find_nth_by_id(PCI_VENDOR_INTEL, i915_ids[i].device_id, nth, &pdev);
             nth++) {
            if (!kcmdline_has("rach2.i915")) {
                klog(KLOG_INFO, "i915",
                     "%02x:%02x.%x: %04x left to the firmware's framebuffer; rach2.i915 "
                     "binds it",
                     (unsigned)pdev.bus, (unsigned)pdev.device, (unsigned)pdev.function,
                     (unsigned)pdev.device_id);
                continue;
            }
            err = i915_pci_probe(&pdev);
            if (err == 0) {
                bound++;
            } else if (err != -ENODEV) {
                klog(KLOG_ERR, "i915", "%02x:%02x.%x: error %d", (unsigned)pdev.bus,
                     (unsigned)pdev.device, (unsigned)pdev.function, (int)err);
            }
        }
    }
    if (kcmdline_has("rach2.i915fake")) {
        err = i915_fake_probe();
        if (err > 0) {
            bound += err;
        }
    }
    return bound > 0 ? bound : -ENODEV;
}
