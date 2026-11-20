/*
 * i915_handoff.c: the Intel graphics driver's release of a device to
 * another driver, vfio-pci for a virtual machine above all, and its
 * bring-up again from a reset device, through /sys/bus/pci/drivers/i915.
 */

#include "i915.h"

#include "../../cmdline.h"
#include "../../display/backlight.h"
#include "../../display/fbcon.h"
#include "../../fs/synthfs.h"
#include "../../fs/sysfs.h"
#include "../../vfio/vfio.h"
#include "../../klog.h"
#include "../../lib/kerrno.h"
#include "../../lib/kstring.h"
#include "../../proc/percpu.h"
#include "../../proc/sched.h"
#include "../../rtc.h"
#include "../../timer.h"
#include "../../udelay.h"

/*
 * The driver's directory is Linux's. `unbind` takes the name of a function
 * the driver has, 0000:BB:DD.F, and releases it; `bind` takes the name of
 * one no driver has and brings it up. While a function is bound it has a
 * `driver` link to the directory, the directory a link to it, and its
 * uevent DRIVER=i915, as Linux's driver core shows a binding
 * (`sysfs_pci_bind`). The directory comes with the first device the driver
 * binds, so a kernel booted without `rach2.i915` has none.
 *
 * The release leaves nothing of the driver on the device, in the order of
 * Linux's i915_driver_remove, with the device held awake throughout as
 * Linux holds a wakeref: RPS and RC6 off, as intel_gt_suspend_late turns
 * them off before its reset, so that no park after the reset sends the GT
 * into RC6; the display off by one commit (drm_atomic_helper_shutdown),
 * which takes the planes, pipes, ports, clocks and the panel down as a
 * mode set does; every ring stopped and the GPU reset under the GEM lock,
 * as intel_engines_release does before the rings' pages go back, so that
 * the chip reads none of them again; then the device unplugged from the
 * DRM core, which fails what was queued, masks every interrupt, gives the
 * message and its vector back and frees the rings. The commit lock is
 * held from the commit to the unplug and the GEM lock from the reset to
 * it, so that no master lights a pipe and no hangcheck starts a ring
 * again between them; Linux unplugs first, which keeps them out the same
 * way. Bus mastering goes off and the function is reset, as the bring-up
 * resets it, since the GPU goes on reaching memory until a reset stops
 * it, and its pages, the GTT's scratch page among them, go back with the
 * last reference. Then the kernel's claim on the function is given up,
 * which is what vfio-pci's bind waits for, and last the unbind uevent is
 * sent, as Linux's driver core sends it once the driver has let go.
 * Programs that still hold the card keep a record that reaches no
 * hardware: its registers read all ones and take no write
 * (`i915_pci_unmap`).
 *
 * Two things outside the driver drew through the device and stop with it.
 * The console may still be on the loader's framebuffer, which the Lenovo's
 * firmware put in the aperture; it is told that the screen has gone. The
 * boot's backlight driver writes the PCH's registers through a mapping of
 * its own, and takes the backlight back when the driver's display goes;
 * right after the unplug, before the function's reset takes the registers
 * away, the class is given a stand-in that writes nothing until the
 * driver binds again.
 *
 * The bring-up starts from a reset, whatever the last owner left: the
 * function's own reset (pci_reset_function), then the panel's power cycle,
 * since the reset drops the panel's power without the sequencer's count of
 * it, then the probe. The probe finds no pipe lit, no plane to keep in the
 * GTT, the PWM off and the panel dark, and the console, which lost its
 * screen at the release, sets its mode at once.
 */

#define PCI_VENDOR_INTEL 0x8086

/* The devices the driver has at once, as i915_pci.c's table. */
#define HANDOFF_DEVICES 4

/* The longest name of a function, "0000:BB:DD.F", and its NUL. */
#define HANDOFF_NAME 13

/* The eDP specification's T11 + T12, in milliseconds: the least a panel is
 * left without power before it is powered again, which stands until a
 * bound device has given its own. */
#define HANDOFF_POWER_CYCLE_MS 510u

/* A bound device, which /sys shows bound to the driver. */
static struct handoff_slot {
    struct drm_i915_private *i915; /* NULL for a free slot */
    int linked;                    /* shown bound to i915 in /sys */
    int announced;                 /* its "bind" uevent sent */
} slots[HANDOFF_DEVICES];

static int sys_dir;       /* /sys/bus/pci/drivers/i915, or 0 */
static int busy;          /* a release or a bring-up sleeps */
static int release_event; /* the unplug of `releasing` left its unbind uevent to the release */
static struct drm_i915_private *releasing; /* the device a release unplugs, or NULL */
static int stand_in_level;                 /* the level the stand-in backlight was last given */

/* The panel's power cycle, the longest a released device had. */
static uint32_t power_cycle_ms = HANDOFF_POWER_CYCLE_MS;

/** The function's devfn, device << 3 | function. */
static uint8_t devfn_of(const pci_device_t *pdev)
{
    return (uint8_t)(pdev->device << 3 | pdev->function);
}

/** Write the name of the function `bus` and `devfn`, 0000:BB:DD.F, into
 * `out` of HANDOFF_NAME bytes. */
static void bdf_name(uint8_t bus, uint8_t devfn, char *out)
{
    static const char hex[] = "0123456789abcdef";

    k_memcpy(out, "0000:", 5);
    out[5] = hex[bus >> 4];
    out[6] = hex[bus & 15u];
    out[7] = ':';
    out[8] = hex[(devfn >> 3) >> 4];
    out[9] = hex[(devfn >> 3) & 15u];
    out[10] = '.';
    out[11] = hex[devfn & 7u];
    out[12] = '\0';
}

/** Whether the `n` bytes at `text` are the whole of the string `word`. */
static int text_is(const char *text, uint32_t n, const char *word)
{
    uint32_t i;

    for (i = 0; i < n; i++) {
        if (word[i] == '\0' || text[i] != word[i]) {
            return 0;
        }
    }
    return word[n] == '\0';
}

/** The value of the hexadecimal digit `c`, or -1. */
static int hex_value(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

/**
 * Read the name of a function at `buf`, `len` bytes with perhaps a newline
 * or spaces after it, as a write of `bind` or `unbind` gives it.
 *
 * @return 0 with `*bus` and `*devfn`, or -1 for text that is no name of a
 *         function of segment 0.
 */
static int bdf_parse(const char *buf, uint32_t len, uint8_t *bus, uint8_t *devfn)
{
    int b1, b0, d1, d0, f;

    while (len > 0 && (buf[len - 1] == '\n' || buf[len - 1] == ' ')) {
        len--;
    }
    if (len != HANDOFF_NAME - 1 || !text_is(buf, 5, "0000:") || buf[7] != ':' || buf[10] != '.') {
        return -1;
    }
    b1 = hex_value(buf[5]);
    b0 = hex_value(buf[6]);
    d1 = hex_value(buf[8]);
    d0 = hex_value(buf[9]);
    f = hex_value(buf[11]);
    if (b1 < 0 || b0 < 0 || d1 < 0 || d0 < 0 || f < 0 || d1 > 1 || f > 7) {
        return -1;
    }
    *bus = (uint8_t)(b1 << 4 | b0);
    *devfn = (uint8_t)((d1 << 4 | d0) << 3 | f);
    return 0;
}

/** The slot of the bound function `bus` and `devfn`, or NULL. */
static struct handoff_slot *slot_of(uint8_t bus, uint8_t devfn)
{
    int i;

    for (i = 0; i < HANDOFF_DEVICES; i++) {
        if (slots[i].i915 && slots[i].i915->pci.bus == bus &&
            devfn_of(&slots[i].i915->pci) == devfn) {
            return &slots[i];
        }
    }
    return 0;
}

/** Whether the deadline at `arg`, in ticks, has passed. */
static int deadline_passed(void *arg)
{
    return rtc_ticks_now() >= *(const uint64_t *)arg;
}

/** Wait `ms` milliseconds at least: a process sleeps to the tick after,
 * giving up the big lock, and anything else spins. */
static void handoff_msleep(unsigned int ms)
{
    static wait_queue_t sleepers;
    static int ready;
    uint64_t due, saved;

    if (ms == 0) {
        return;
    }
    if (!current_proc || !sched_is_enabled()) {
        while (ms-- > 0) {
            k_udelay(1000);
        }
        return;
    }
    if (!ready) {
        wait_queue_init(&sleepers);
        ready = 1;
    }
    due = rtc_ticks_now() + drm_div64((uint64_t)ms * TICK_HZ + 999u, 1000u) + 1u;
    saved = current_proc->wakeup_tick;
    current_proc->wakeup_tick = due;
    (void)wq_wait_event_nointr(&sleepers, deadline_passed, &due);
    current_proc->wakeup_tick = saved;
}

/* ---- the release ---- */

/** Whether a program sleeps with a window of the aperture in its hands,
 * as a GTT pread or pwrite does through a fault of its copy: it would
 * reach the aperture again after the device has gone. */
static int aperture_held(const struct drm_i915_private *i915)
{
    int i;

    for (i = 0; i < I915_APERTURE_WINDOWS; i++) {
        if (i915->gem.aperture.win[i].users != 0) {
            return 1;
        }
    }
    return 0;
}

/** Whether the console's first screen, the loader's framebuffer, is shown
 * through the aperture of `i915`, as the Lenovo's firmware leaves it. */
static int boot_screen_in_aperture(const struct drm_i915_private *i915)
{
    const struct boot_framebuffer *fb = fbcon_boot_screen();

    return fbcon_active && (fb->type == BOOT_FB_RGB || fb->type == BOOT_FB_INDEXED) &&
           i915->gmadr_phys != 0 && fb->addr >= i915->gmadr_phys &&
           fb->addr - i915->gmadr_phys < i915->ggtt.mappable_end;
}

/** The stand-in backlight's level: what it was last given. */
static int stand_in_get(void *ctx)
{
    (void)ctx;
    return stand_in_level;
}

/** Keep the level the class gives the stand-in; nothing is written. */
static void stand_in_set(void *ctx, int level)
{
    (void)ctx;
    stand_in_level = level;
}

static const struct backlight_ops stand_in_ops = {stand_in_get, stand_in_set, NULL, NULL};

/**
 * Put "intel_backlight" on a stand-in that writes no register, with the
 * levels it has, until the driver binds again and takes it over: the
 * boot's driver, which took it back at the unplug, would write the PCH's
 * PWM and the panel's enable on a device that is another owner's.
 */
static void backlight_stand_in(void)
{
    char text[16];
    int i = backlight_find("intel_backlight", 15), n, k, max = 0;

    if (i < 0) {
        return;
    }
    n = backlight_show(i, BACKLIGHT_LEVELS, text, (int)sizeof(text));
    for (k = 0; k < n && k < (int)sizeof(text) && text[k] >= '0' && text[k] <= '9'; k++) {
        max = max * 10 + (text[k] - '0');
    }
    if (max > 0) {
        stand_in_level = 0;
        (void)backlight_rebind("intel_backlight", max, &stand_in_ops, 0);
    }
}

/**
 * Turn RPS and RC6 on again on a device whose release was refused after
 * `i915_gt_pm_disable`, unless an unplug has taken it meanwhile. Takes the
 * GEM lock, so the caller holds no commit lock.
 */
static void handoff_pm_restore(struct drm_i915_private *i915)
{
    int taken = i915_gem_lock(i915);

    if (!i915->drm.unplugged) {
        i915_gt_pm_restore(i915);
    }
    i915_gem_unlock(i915, taken);
}

/**
 * Reset the released function `pdev`, named `name`, with bus mastering
 * off and the kernel's claim still held, as the bring-up does: the GPU
 * goes on reaching memory after the release until a reset stops it, and
 * its pages, the GTT's scratch page among them, go back at the last
 * reference. Linux keeps an unopened vfio-pci device quiet in D3hot; this
 * gives the same from the release on. The fake chip has no state to reset.
 * May sleep.
 *
 * @return 1 when the function was reset; 0 for the fake chip; -1, with the
 *         failure logged, for a function with no reset or one that failed.
 */
static int handoff_release_reset(const pci_device_t *pdev, const char *name)
{
    int32_t err;

    if (pdev->vendor_id != PCI_VENDOR_INTEL || !i915_id_lookup(pdev->device_id)) {
        return 0;
    }
    err = pci_reset_function(pdev, handoff_msleep);
    if (err != 0) {
        klog(KLOG_WARN, "i915", "handoff: %s: not reset at the release: error %d", name, (int)err);
        return -1;
    }
    return 1;
}

/**
 * Keep the GTT's scratch page of `i915`, named `name`, whose function the
 * release could not reset: the GPU may still reach it, so the record's
 * release does not free it and the frame is never used again, as a status
 * page whose writes were not masked is kept.
 */
static void handoff_scratch_keep(struct drm_i915_private *i915, const char *name)
{
    if (i915->ggtt.scratch == 0) {
        return;
    }
    klog(KLOG_WARN, "i915", "handoff: %s: scratch page kept at 0x%lx: the function was not reset",
         name, (unsigned long)i915->ggtt.scratch);
    i915->ggtt.scratch = 0;
}

/**
 * Give back what the release took when it is refused: the GEM lock and
 * the commit lock when `gem` and `locked` say it holds them, RPS and RC6
 * when `pm_off` says it turned them off, and the device's wakeref; the
 * refusal logged and the console told to look again. Returns `err`.
 */
static int32_t handoff_refused(struct drm_i915_private *i915, const char *name, int32_t err,
                               int gem, int locked, int pm_off)
{
    struct drm_device *dev = &i915->drm;

    if (gem >= 0) {
        i915_gem_unlock(i915, gem);
    }
    if (locked) {
        drm_modeset_unlock(dev);
    }
    if (pm_off) {
        handoff_pm_restore(i915);
    }
    klog(KLOG_ERR, "i915", "handoff: %s: not released: error %d", name, (int)err);
    if (!dev->unplugged) {
        drm_client_dev_hotplug(dev);
    }
    i915_rpm_put(i915);
    drm_dev_put(dev);
    return err;
}

/**
 * Release the device `i915` so that another driver may have its function,
 * as the comment at the top says. The device is held awake throughout, as
 * Linux's i915_driver_remove holds a wakeref, so that no write of the
 * release is lost to a runtime suspend. The commit lock is held from the
 * display's commit to the unplug, so that no master's commit lights a
 * pipe between them, and the GEM lock from the GPU's reset to the unplug,
 * so that no hangcheck resets the GPU or starts a ring again after it.
 * May sleep.
 *
 * @return 0; the commit's error; -EBUSY while a program sleeps with a
 *         window of the aperture; -EIO or -ETIMEDOUT when the GPU did not
 *         reset. On an error the device stays bound, RPS and RC6 on again
 *         and its display off until the console or a master sets a mode.
 */
static int32_t handoff_release(struct drm_i915_private *i915)
{
    struct drm_device *dev = &i915->drm;
    pci_device_t pdev = i915->pci;
    char name[HANDOFF_NAME];
    int32_t err;
    int taken, pm_off = 0, locked = 0, gem = -1, reset;

    bdf_name(pdev.bus, devfn_of(&pdev), name);
    drm_dev_get(dev);
    i915_rpm_get(i915);
    err = aperture_held(i915) ? -EBUSY : 0;
    if (err == 0) {
        /* Linux's intel_gt_suspend_late: RPS and RC6 off before the last
         * reset. Once they are off a park or an unpark writes nothing, so
         * none after the reset sends the GT into RC6. Under the GEM lock,
         * as every park, and before the commit lock, which comes first. */
        taken = i915_gem_lock(i915);
        if (!dev->unplugged) {
            i915_gt_pm_disable(i915);
            pm_off = 1;
        }
        i915_gem_unlock(i915, taken);
        drm_modeset_lock_nointr(dev);
        locked = 1;
        err = drm_atomic_helper_shutdown(dev);
    }
    if (err == 0 && dev->unplugged) {
        err = -ENODEV;
    }
    if (err == 0 && aperture_held(i915)) {
        err = -EBUSY;
    }
    if (err == 0) {
        gem = i915_gem_lock(i915);
        err = i915_gt_quiesce(i915);
    }
    if (err != 0) {
        return handoff_refused(i915, name, err, gem, locked, pm_off);
    }
    if (i915->display.dp.pps.ready && i915->display.dp.pps.power_cycle_ms > power_cycle_ms) {
        power_cycle_ms = i915->display.dp.pps.power_cycle_ms;
    }
    if (boot_screen_in_aperture(i915)) {
        drm_client_boot_screen_gone();
    }
    intel_dp_aux_dev_unregister(i915);
    intel_i2c_unregister(i915);
    releasing = i915;
    drm_dev_unplug(dev);
    i915_gem_unlock(i915, gem);
    drm_modeset_unlock(dev);
    /* The unplug gave the backlight back to the boot's driver, whose
     * registers the function's reset takes away. */
    if (pdev.vendor_id == PCI_VENDOR_INTEL && pdev.bus == 0 && pdev.device == 2 &&
        pdev.function == 0) {
        backlight_stand_in();
    }
    i915_aperture_fini(&i915->gem.aperture);
    i915->gem.aperture.size = 0;
    i915_pci_unmap(&i915->hw);
    pci_disable_bus_master(&pdev);
    reset = handoff_release_reset(&pdev, name);
    if (reset < 0) {
        handoff_scratch_keep(i915, name);
    } else if (reset > 0) {
        i915_ggtt_carry_drop(&pdev);
    }
    pci_unclaim(pdev.bus, devfn_of(&pdev), PCI_OWNER_KERNEL);
    releasing = 0;
    /* Linux's driver core sends KOBJ_UNBIND last, once the function is
     * free for the next driver's probe. */
    if (release_event) {
        release_event = 0;
        sysfs_pci_uevent((int)pdev.bus << 8 | devfn_of(&pdev), "unbind");
    }
    klog(KLOG_INFO, "i915",
         "handoff: %s: released: the display off, the GPU reset, interrupts and bus mastering "
         "off%s",
         name, reset > 0 ? ", the function reset" : "");
    i915_rpm_put(i915);
    drm_dev_put(dev);
    return 0;
}

/* ---- the bring-up ---- */

/** The function `bus` and `devfn` of the buses, into `*out`: 1, or 0. */
static int function_find(uint8_t bus, uint8_t devfn, pci_device_t *out)
{
    int nth;

    for (nth = 0; pci_nth(nth, out); nth++) {
        if (out->bus == bus && devfn_of(out) == devfn) {
            return 1;
        }
    }
    return 0;
}

/** After a bind that failed: bus mastering off and the kernel's claim on
 * the function, which the bind's first step took, given up, so that the
 * function can be bound again, by this driver or another. */
static void bind_undo(const pci_device_t *pdev)
{
    pci_device_t dev = *pdev;

    pci_disable_bus_master(&dev);
    pci_unclaim(dev.bus, devfn_of(&dev), PCI_OWNER_KERNEL);
}

/**
 * Bring the function `pdev` up from a reset, as the comment at the top
 * says: the chip through its registers, or the fake chip on an `edu`
 * function under `rach2.i915fake`, which has no state of an owner's to
 * reset. May sleep.
 *
 * @return 0, -ENODEV for a function the driver does not bind, the reset's
 *         error, or the bind's.
 */
static int32_t handoff_bring_up(const pci_device_t *pdev)
{
    char name[HANDOFF_NAME];
    int32_t err;
    int reset;

    bdf_name(pdev->bus, devfn_of(pdev), name);
    if (pdev->vendor_id != PCI_VENDOR_INTEL || !i915_id_lookup(pdev->device_id)) {
        err = kcmdline_has("rach2.i915fake") ? i915_fake_bind(pdev) : -ENODEV;
        if (err != 0) {
            bind_undo(pdev);
        } else {
            klog(KLOG_INFO, "i915", "handoff: %s: bound", name);
        }
        return err;
    }
    reset = pci_reset_method(pdev) != PCI_RESET_NONE;
    if (!reset) {
        klog(KLOG_WARN, "i915", "handoff: %s: no reset; bound as its last owner left it", name);
    } else {
        err = pci_reset_function(pdev, handoff_msleep);
        if (err != 0) {
            klog(KLOG_ERR, "i915", "handoff: %s: not reset: error %d", name, (int)err);
            return err;
        }
        i915_ggtt_carry_drop(pdev);
        handoff_msleep(power_cycle_ms);
    }
    err = i915_pci_probe(pdev);
    if (err != 0) {
        bind_undo(pdev);
        klog(KLOG_ERR, "i915", "handoff: %s: not bound: error %d", name, (int)err);
    } else {
        klog(KLOG_INFO, "i915", "handoff: %s: bound %s a reset", name, reset ? "from" : "without");
    }
    return err;
}

/* ---- /sys ---- */

/**
 * /sys/bus/pci/drivers/i915/unbind: release the function named. -ENODEV
 * for a name that is none or a function the driver does not have, -EBUSY
 * while another write of the directory sleeps, or the release's error.
 */
static int32_t store_unbind(void *obj, int arg, const char *buf, uint32_t len)
{
    struct handoff_slot *slot;
    uint8_t bus, devfn;
    int32_t err;

    (void)obj;
    (void)arg;
    if (bdf_parse(buf, len, &bus, &devfn) != 0 || (slot = slot_of(bus, devfn)) == 0) {
        return -ENODEV;
    }
    if (busy) {
        return -EBUSY;
    }
    busy = 1;
    err = handoff_release(slot->i915);
    busy = 0;
    return err != 0 ? err : (int32_t)len;
}

/**
 * /sys/bus/pci/drivers/i915/bind: bring the function named up. -ENODEV for
 * a name that is none, a function that is not there or one whose
 * driver_override names another driver; -EBUSY for one a driver has, this
 * one among them, or while another write of the directory sleeps; else the
 * bring-up's error.
 */
static int32_t store_bind(void *obj, int arg, const char *buf, uint32_t len)
{
    pci_device_t pdev;
    uint8_t bus, devfn;
    int32_t err;

    (void)obj;
    (void)arg;
    if (bdf_parse(buf, len, &bus, &devfn) != 0 || !function_find(bus, devfn, &pdev) ||
        !vfio_override_allows((int)bus << 8 | devfn, "i915")) {
        return -ENODEV;
    }
    if (busy || slot_of(bus, devfn) || pci_owner(bus, devfn) != 0) {
        return -EBUSY;
    }
    busy = 1;
    err = handoff_bring_up(&pdev);
    busy = 0;
    return err != 0 ? err : (int32_t)len;
}

static const struct sysfs_attr attr_bind = {"bind", 0200u, 0, store_bind};
static const struct sysfs_attr attr_unbind = {"unbind", 0200u, 0, store_unbind};

/** The directory `name` below `parent`, made when it is not there yet. */
static int dir_at(int parent, const char *name)
{
    int h = sysfs_node_child(parent, name, (int)k_strlen(name));

    return h > 0 ? h : sysfs_add_dir(parent, name);
}

/** Make the driver's directory and its two files, once. */
static void sys_start(void)
{
    int drivers;

    if (sys_dir > 0) {
        return;
    }
    drivers = dir_at(sysfs_anchor(SYSFS_BUS_PCI, 0), "drivers");
    sys_dir = drivers > 0 ? dir_at(drivers, "i915") : drivers;
    if (sys_dir <= 0) {
        klog(KLOG_WARN, "i915", "handoff: /sys/bus/pci/drivers/i915 not made: error %d", sys_dir);
        sys_dir = 0;
        return;
    }
    (void)sysfs_add_attr(sys_dir, &attr_bind, 0, 0);
    (void)sysfs_add_attr(sys_dir, &attr_unbind, 0, 0);
}

int32_t i915_handoff_link(struct drm_i915_private *i915)
{
    struct handoff_slot *slot = 0;
    int i, err;

    for (i = 0; i < HANDOFF_DEVICES && !slot; i++) {
        if (!slots[i].i915) {
            slot = &slots[i];
        }
    }
    if (!slot) {
        klog(KLOG_WARN, "i915", "handoff: refused: every one of %d records is taken",
             HANDOFF_DEVICES);
        return -ENOSPC;
    }
    sys_start();
    err = sysfs_pci_bind((int)i915->pci.bus << 8 | devfn_of(&i915->pci), "i915");
    if (err != 0) {
        klog(KLOG_WARN, "i915", "handoff: refused: driver link not added: error %d", err);
        return err;
    }
    slot->i915 = i915;
    slot->linked = 1;
    slot->announced = 0;
    return 0;
}

void i915_handoff_bound(struct drm_i915_private *i915)
{
    int i;

    for (i = 0; i < HANDOFF_DEVICES; i++) {
        if (slots[i].i915 == i915 && slots[i].linked && !slots[i].announced) {
            slots[i].announced = 1;
            sysfs_pci_uevent((int)i915->pci.bus << 8 | devfn_of(&i915->pci), "bind");
        }
    }
}

void i915_handoff_unbound(struct drm_i915_private *i915)
{
    int key = (int)i915->pci.bus << 8 | devfn_of(&i915->pci), i;

    for (i = 0; i < HANDOFF_DEVICES; i++) {
        if (slots[i].i915 == i915) {
            if (slots[i].linked) {
                sysfs_pci_unbind(key);
            }
            if (slots[i].announced && releasing == i915) {
                release_event = 1;
            } else if (slots[i].announced) {
                sysfs_pci_uevent(key, "unbind");
            }
            slots[i].i915 = 0;
            slots[i].linked = 0;
            slots[i].announced = 0;
        }
    }
}
