/*
 * intel_i2c_dev.c: the display's I2C buses as I2C adapters (i2c.h), each
 * GMBUS pin's and each DisplayPort channel's I2C over AUX, with the
 * connectors' `ddc` links, so that /dev/i2c-N reaches them as on Linux.
 */

#include "i915.h"
#include "intel_dp.h"

#include "../../fs/synthfs.h"
#include "../../fs/sysfs.h"
#include "../../klog.h"
#include "../../lib/kerrno.h"
#include "../../lib/kstring.h"

/** The bits an adapter here gives I2C_FUNCS: plain I2C and the SMBus
 * transfers made of it, as Linux's GMBUS and DP helpers give them less the
 * ten-bit addresses and the block reads, which neither bus does here. */
#define INTEL_I2C_FUNC (I2C_FUNC_I2C | I2C_FUNC_SMBUS_EMUL)

/**
 * Run `n` messages on `bus` of `i915` under the device's mode lock, which
 * an AUX transfer needs and a commit's GMBUS reads take before the bus's.
 * `n`, or -EINVAL for a ten-bit address, which neither bus sends, -ENODEV
 * once the device is unplugged, or the bus's -errno.
 */
static int32_t bus_xfer(struct drm_i915_private *i915, const struct intel_i2c_bus *bus,
                        struct i2c_msg *msgs, int n)
{
    struct intel_i2c_msg m[I2C_RDWR_MAX_MSGS];
    struct drm_device *dev = &i915->drm;
    int32_t ret;
    int i;

    if (n <= 0 || n > (int)I2C_RDWR_MAX_MSGS) {
        return -EINVAL;
    }
    for (i = 0; i < n; i++) {
        if ((msgs[i].flags & I2C_M_TEN) || msgs[i].addr > 0x7fu) {
            return -EINVAL;
        }
        m[i].addr = (uint8_t)msgs[i].addr;
        m[i].read = (msgs[i].flags & I2C_M_RD) != 0;
        m[i].len = msgs[i].len;
        m[i].buf = msgs[i].buf;
    }
    if ((ret = drm_modeset_lock(dev)) != 0) {
        return ret;
    }
    ret = dev->unplugged ? -ENODEV : bus->xfer(bus->ctx, m, n);
    drm_modeset_unlock(dev);
    return ret;
}

/** A GMBUS pin's transfer (`bus_xfer`). */
static int32_t gmbus_adapter_xfer(struct i2c_adapter *adap, struct i2c_msg *msgs, int n)
{
    struct intel_gmbus *g = (struct intel_gmbus *)adap->priv;

    return bus_xfer(g->i915, &g->bus, msgs, n);
}

/** A DisplayPort channel's I2C over AUX transfer (`bus_xfer`). */
static int32_t dp_adapter_xfer(struct i2c_adapter *adap, struct i2c_msg *msgs, int n)
{
    struct intel_dp *dp = (struct intel_dp *)adap->priv;
    struct intel_i2c_bus bus;

    intel_dp_i2c_bus(dp, &bus);
    return bus_xfer(dp->i915, &bus, msgs, n);
}

/** Hold the DRM device of a GMBUS pin while its node is open. */
static void gmbus_adapter_hold(struct i2c_adapter *adap, int get)
{
    struct drm_device *dev = &((struct intel_gmbus *)adap->priv)->i915->drm;

    if (get) {
        drm_dev_get(dev);
    } else {
        drm_dev_put(dev);
    }
}

/** Hold the DRM device of a DisplayPort channel while its node is open. */
static void dp_adapter_hold(struct i2c_adapter *adap, int get)
{
    struct drm_device *dev = &((struct intel_dp *)adap->priv)->i915->drm;

    if (get) {
        drm_dev_get(dev);
    } else {
        drm_dev_put(dev);
    }
}

/** Add GMBUS pin `g` below the PCI function's directory `parent`, named as
 * Linux names it, "i915 gmbus dpb". */
static void gmbus_adapter_add(struct intel_gmbus *g, int parent)
{
    struct i2c_adapter *a = &g->adapter;
    int32_t rc;

    k_memset(a, 0, sizeof(*a));
    a->xfer = gmbus_adapter_xfer;
    a->hold = gmbus_adapter_hold;
    a->priv = g;
    a->func = INTEL_I2C_FUNC;
    k_memcpy(a->name, "i915 ", 5);
    (void)k_strncpy(a->name + 5, g->name, sizeof(a->name) - 6);
    if ((rc = i2c_add_adapter(a, parent)) != 0) {
        klog(KLOG_WARN, "i915", "%s: no i2c adapter: error %d", g->name, (int)rc);
    }
}

/** Add `dp`'s I2C over AUX below its connector's directory, named as
 * Linux's intel_dp_aux_init names the channel, "AUX A/DDI A/PHY A". */
static void dp_adapter_add(struct intel_dp *dp)
{
    struct i2c_adapter *a = &dp->ddc;
    int32_t rc;

    k_memset(a, 0, sizeof(*a));
    a->xfer = dp_adapter_xfer;
    a->hold = dp_adapter_hold;
    a->priv = dp;
    a->func = INTEL_I2C_FUNC;
    k_memcpy(a->name, "AUX ?/DDI ?/PHY ?", 18);
    a->name[4] = (char)('A' + dp->aux_port);
    a->name[10] = a->name[16] = (char)('A' + dp->port);
    if ((rc = i2c_add_adapter(a, drm_sysfs_connector_dir(dp->connector))) != 0) {
        klog(KLOG_WARN, "i915", "aux: %s: no i2c adapter: error %d", dp->connector->name, (int)rc);
    }
}

/** Link `connector`'s `ddc` to `a`, Linux's drm_sysfs_connector_add_late,
 * keeping the link's handle in the next free slot of `links`. */
static void ddc_link(struct drm_connector *connector, const struct i2c_adapter *a, int *links)
{
    int dir = drm_sysfs_connector_dir(connector), i, h;

    if (dir <= 0 || a->nr < 0 || a->dir <= 0) {
        return;
    }
    for (i = 0; i < 7 && links[i]; i++) {
    }
    if (i < 7 && (h = sysfs_add_link(dir, "ddc", a->dir)) > 0) {
        links[i] = h;
    }
}

/** Whether `dp` is a channel whose connector the device registered. */
static int dp_registered(struct drm_i915_private *i915, const struct intel_dp *dp)
{
    return dp->present && dp->connector && dp->connector->dev == &i915->drm;
}

/** Add the adapter of each registered DisplayPort channel and give its
 * connector, and each HDMI connector, its `ddc` link. */
static void outputs_add(struct drm_i915_private *i915)
{
    struct intel_display *d = &i915->display;
    struct intel_ddi_port *p;
    uint32_t i;

    if (dp_registered(i915, &d->dp)) {
        dp_adapter_add(&d->dp);
        ddc_link(d->dp.connector, &d->dp.ddc, d->ddc_links);
    }
    for (i = 0; i < sizeof(d->ddi) / sizeof(d->ddi[0]); i++) {
        p = &d->ddi[i];
        if (!p->present) {
            continue;
        }
        if (p->has_dp && dp_registered(i915, &p->dp)) {
            dp_adapter_add(&p->dp);
            ddc_link(p->dp.connector, &p->dp.ddc, d->ddc_links);
        }
        if (p->has_hdmi && p->hdmi_connector.dev == &i915->drm &&
            intel_gmbus_is_valid_pin(p->hdmi.ddc_pin)) {
            ddc_link(&p->hdmi_connector, &d->gmbus[p->hdmi.ddc_pin].adapter, d->ddc_links);
        }
    }
}

void intel_i2c_register(struct drm_i915_private *i915)
{
    const pci_device_t *p = &i915->pci;
    int parent = sysfs_anchor(SYSFS_PCI_DEVICE, p->bus << 8 | p->device << 3 | p->function);
    int pin;

    if (!(i915->drm.features & DRIVER_MODESET) || !i915->display.ready) {
        return;
    }
    for (pin = 0; pin < GMBUS_NUM_PINS; pin++) {
        if (i915->display.gmbus[pin].valid && parent > 0) {
            gmbus_adapter_add(&i915->display.gmbus[pin], parent);
        }
    }
    outputs_add(i915);
}

void intel_i2c_unregister(struct drm_i915_private *i915)
{
    struct intel_display *d = &i915->display;
    uint32_t i;
    int pin;

    for (i = 0; i < 7; i++) {
        sysfs_remove(d->ddc_links[i]);
        d->ddc_links[i] = 0;
    }
    i2c_del_adapter(&d->dp.ddc);
    for (i = 0; i < sizeof(d->ddi) / sizeof(d->ddi[0]); i++) {
        i2c_del_adapter(&d->ddi[i].dp.ddc);
    }
    for (pin = 0; pin < GMBUS_NUM_PINS; pin++) {
        i2c_del_adapter(&d->gmbus[pin].adapter);
    }
}
