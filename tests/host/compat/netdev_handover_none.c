/* What netdev.c's hand-over of a card asks of PCI, /sys and VFIO, for a
 * host test that links netdev.c and hands no card over: /sys refuses every
 * entry, PCI has no function and VFIO groups none. */
#include "../../../rach2/sys/fs/sysfs.h"
#include "../../../rach2/sys/lib/kerrno.h"
#include "../../../rach2/sys/pci.h"
#include "../../../rach2/sys/udelay.h"
#include "../../../rach2/sys/vfio/vfio.h"

/** No anchor is made. */
int sysfs_anchor(int node, int key)
{
    (void)node;
    (void)key;
    return -ENOSPC;
}

/** No directory is made. */
int sysfs_add_dir(int parent, const char *name)
{
    (void)parent;
    (void)name;
    return -ENOSPC;
}

/** No file is made. */
int sysfs_add_attr(int parent, const struct sysfs_attr *attr, void *obj, int arg)
{
    (void)parent;
    (void)attr;
    (void)obj;
    (void)arg;
    return -ENOSPC;
}

/** No entry is there. */
int sysfs_node_child(int dir, const char *name, int len)
{
    (void)dir;
    (void)name;
    (void)len;
    return 0;
}

/** No driver is shown. */
int sysfs_pci_bind(int key, const char *driver)
{
    (void)key;
    (void)driver;
    return -ENOSPC;
}

/** Nothing was shown. */
void sysfs_pci_unbind(int key)
{
    (void)key;
}

/** No event is sent. */
int32_t sysfs_pci_uevent(int key, const char *action)
{
    (void)key;
    (void)action;
    return 0;
}

/** No function is claimed. */
void pci_unclaim(uint8_t bus, uint8_t devfn, const char *owner)
{
    (void)bus;
    (void)devfn;
    (void)owner;
}

/** No function has an owner. */
const char *pci_owner(uint8_t bus, uint8_t devfn)
{
    (void)bus;
    (void)devfn;
    return 0;
}

/** Every function is in D0. */
int pci_power_state(const pci_device_t *dev)
{
    (void)dev;
    return PCI_D0;
}

/** No function changes its state. */
int pci_set_power_state(const pci_device_t *dev, int state)
{
    (void)dev;
    (void)state;
    return -ENODEV;
}

/** No function is grouped, so none has a driver_override. */
int vfio_override_allows(int key, const char *driver)
{
    (void)key;
    (void)driver;
    return 1;
}

/** No time passes. */
void k_udelay(uint32_t usec)
{
    (void)usec;
}
