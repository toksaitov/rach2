/*
 * vfio.h: VFIO's own declarations, which group.c, container.c, pci.c,
 * igd.c and hw.c share: the groups of devices the IOMMU tells apart, the
 * containers of their DMA mappings, and the PCI devices given to vfio-pci.
 * The kernel starts it through `vfio_start`; KVM checks its descriptors.
 */

#ifndef _VFIO_H_
#define _VFIO_H_

#include "../io/anon.h"
#include "../lib/kint.h"
#include "../mm/pin.h"
#include "../pci.h"
#include "vfio_abi.h"

struct eventfd;
struct eventfd_hook;
struct iommu_domain;
struct mm;
struct file_ops;
struct devfs_node;
struct fd_entry;

/** Whether `e` is a live VFIO group or device descriptor. Under the big
 * lock; the caller holds the descriptor or its open file description. */
int vfio_file_is_valid(const struct fd_entry *e);

/** Whether `e` is a VFIO device descriptor; used by group.c's classifier. */
int vfio_device_file_is_valid(const struct fd_entry *e);

#define VFIO_GROUPS_MAX  64
#define VFIO_DEVICES_MAX 128
#define VFIO_DMA_MAX     65535u /* Linux's dma_entry_limit */
#define VFIO_MSIX_MAX    2048u  /* the most entries an MSI-X table has */

/* The owner vfio-pci records for a device given to it (pci_claim). */
#define VFIO_DRIVER "vfio-pci"

/* One interrupt of a device: its line, its message or an MSI-X entry. */
struct vfio_irq {
    struct vfio_device *vdev;
    unsigned int index;      /* the entry of the interrupt's kind */
    int vector;              /* the host's vector, or -1 */
    struct eventfd *trigger; /* signalled on each interrupt, or NULL */
};

/* The regions of an Intel graphics device that Linux's vfio_pci_igd.c
 * adds: its OpRegion, with the extended VBT after it if there is one, and
 * read-only views of the host bridge's and the LPC bridge's configuration
 * spaces, which QEMU's legacy IGD mode reads. */
struct vfio_igd {
    uintptr_t va;      /* the OpRegion, OPREGION_SIZE bytes mapped */
    uintptr_t vbt_va;  /* the extended VBT, mapped apart, or 0 */
    uint32_t vbt_size; /* its bytes, 0 for none */
    uint32_t size;     /* the region's bytes: the OpRegion's and the VBT's */
    uint16_t version;  /* the OpRegion's, as the owner reads it */
    int rvda_patched;  /* RVDA reads as OPREGION_SIZE, the VBT's offset */
    int present;       /* the OpRegion region is there */
    int cfg;           /* the two bridges' regions are there too */
};

/* The regions an IGD adds after VFIO_PCI_NUM_REGIONS, in Linux's order. */
#define VFIO_IGD_OPREGION_INDEX VFIO_PCI_NUM_REGIONS
#define VFIO_IGD_HOST_INDEX     (VFIO_PCI_NUM_REGIONS + 1)
#define VFIO_IGD_LPC_INDEX      (VFIO_PCI_NUM_REGIONS + 2)

/*
 * A PCI function in a group, for the life of the kernel: its identity, the
 * names and files of /sys that VFIO adds for it, whether it is given to
 * vfio-pci, and while it is open the state of its regions and interrupts.
 * `anon` comes first, since its descriptor is an anonymous one.
 */
struct vfio_device {
    struct anon_obj anon;
    pci_device_t pci;
    uint16_t key;      /* bus << 8 | devfn, the key of /sys */
    int group;         /* its group's index */
    char name[16];     /* 0000:BB:DD.F */
    char override[32]; /* driver_override, "" for none */
    int bound;         /* given to vfio-pci */
    unsigned int refs; /* open descriptions and mappings */
    int sys_link;      /* /sys/bus/pci/devices/BDF/iommu_group */
    int sys_driver;    /* BDF/driver while bound, or 0 */
    int sys_drvlink;   /* drivers/vfio-pci/BDF while bound, or 0 */
    int sys_override;
    /* While open: */
    int resetting;
    int reset_method;
    struct pci_saved_state saved;
    uint64_t bar_phys[6];
    uint64_t bar_size[6]; /* 0 for none, and the upper half of a 64-bit BAR */
    int bar_io[6];
    uint32_t vbar[6];     /* the BARs as the owner wrote them */
    uint8_t bar_flags[6]; /* the low bits of each BAR, its kind */
    uint32_t vrom;
    uint64_t rom_phys; /* the ROM's address: its BAR's, or the shadow's */
    uint32_t rom_size; /* 0 for none */
    int rom_shadow;    /* the ROM is the firmware's copy at 0xC0000 */
    int reset_works;   /* a reset succeeded at the open, as Linux's */
    int pm_restore;    /* it loses its state in D3hot: No_Soft_Reset clear */
    int power_low;     /* VFIO put it in D3hot; its BARs are not reached */
    int pm_saved_ok;   /* `pm_saved` holds the state kept on the way to D3hot */
    struct pci_saved_state pm_saved;
    uint8_t cfg_map[256]; /* how each byte of the header is reached: CFG_* */
    uint16_t vcmd_intx;   /* the owner's Interrupt Disable bit */
    uint8_t vcache, vlatency, vline;
    uint8_t msi_cap, msix_cap, pcie_cap, af_cap, pm_cap;
    uint8_t vmsi[24];   /* the MSI capability as the owner sees it */
    uint16_t vmsix_ctl; /* the MSI-X control word's enable and mask bits */
    uint32_t cfg_size;  /* 256, or 4096 with extended space */
    int msix_bar;
    uint32_t msix_offset, msix_size, msix_entries;
    struct pci_window msix_win; /* the table, mapped while MSI-X is on */
    int intx_ok;                /* the device masks its line by Interrupt Disable */
    int irq_type;               /* VFIO_PCI_*_IRQ_INDEX, or -1 for none */
    struct vfio_irq intx;
    int intx_masked;
    struct eventfd *intx_unmask; /* the unmask eventfd, or NULL */
    struct eventfd_hook *unmask_hook;
    struct vfio_irq *msi; /* the messages or MSI-X entries, `nmsi` */
    unsigned int nmsi;
    struct eventfd *err_trigger; /* the error and request eventfds */
    struct eventfd *req_trigger;
    struct vfio_igd igd;
};

/* A group: the devices one IOMMU tells apart from the rest. */
struct vfio_group {
    int id;   /* N of /dev/vfio/N and /sys/kernel/iommu_groups/N */
    int unit; /* the IOMMU unit */
    int ndev;
    int devs[VFIO_DEVICES_MAX]; /* indexes of `vfio_devices` */
    char node_name[16];         /* vfio/N */
    struct devfs_node *node;
    int open;              /* its descriptor is open */
    unsigned int dev_open; /* its devices' references, which hold it */
    struct vfio_container *container;
    int attached; /* its devices are in the container's domain */
    struct vfio_group *next_in_container;
    int sys_dir;
};

/* One mapping of a container: user memory pinned and mapped at an IOVA. */
struct vfio_dma {
    uint64_t iova;
    uint64_t size;
    uint64_t vaddr;
    int prot;                 /* IOMMU_READ and IOMMU_WRITE */
    struct pin *pin;          /* held until the mapping goes, live or taken back */
    struct pin_holder holder; /* told when an unmap takes the pages back */
    struct vfio_container *container;
    struct vfio_dma *next;
};

/* A container, the descriptor of /dev/vfio/vfio: groups that share one
 * IOMMU domain and its mappings. */
struct vfio_container {
    int fd_open;
    int iommu_type; /* VFIO_TYPE1_IOMMU or VFIO_TYPE1v2_IOMMU once set, or 0 */
    struct iommu_domain *domain;
    struct vfio_group *groups;
    unsigned int ndma;
    struct vfio_dma *dmas; /* in IOVA order */
};

extern struct vfio_device vfio_devices[VFIO_DEVICES_MAX];
extern int vfio_ndevices;
extern struct vfio_group vfio_groups[VFIO_GROUPS_MAX];
extern int vfio_ngroups;

/* group.c */
void vfio_start(void);
/** Count attached groups, referenced devices, DMA mappings, live pinned
 * RAM pages and mapped device pages into `counts[5]`, under the big lock. */
void vfio_resource_counts(uint32_t *counts);
int vfio_groups_build(void);
int vfio_group_viable(const struct vfio_group *g);
int vfio_bind(struct vfio_device *d);
int vfio_unbind(struct vfio_device *d);
struct vfio_device *vfio_device_by_name(const char *name, int len);
void vfio_group_put_device(struct vfio_group *g);
int vfio_sysfs_start(void);
int vfio_nodes_start(void);

/* container.c */
extern const struct file_ops vfio_container_ops;
int32_t vfio_container_open(struct devfs_node *node, struct fd_entry *e, int flags);
int vfio_container_attach(struct vfio_container *c, struct vfio_group *g);
void vfio_container_detach(struct vfio_group *g);
int vfio_container_check_extension(const struct vfio_container *c, uintptr_t ext);
void vfio_container_audit(const struct vfio_container *c, const char *name);

/* pci.c */
int32_t vfio_device_open_fd(struct vfio_device *d);
int vfio_device_enable(struct vfio_device *d);
void vfio_device_release(struct vfio_device *d);
void vfio_device_zap(struct vfio_device *d);
void vfio_device_power(struct vfio_device *d, int state);

/* config.c: the configuration space as the owner sees it. A byte of the
 * header is read and written through (CFG_RAW), read through and its
 * writes dropped (CFG_RO), or kept by VFIO (CFG_VIRT). */
#define CFG_RAW  0
#define CFG_RO   1
#define CFG_VIRT 2
void vfio_config_init(struct vfio_device *d);
int32_t vfio_config_rw(struct vfio_device *d, uint32_t pos, uint8_t *buf, uint32_t count,
                       int write);
uint16_t vfio_config_command(const struct vfio_device *d, uint16_t cmd);

/* irq.c */
uint32_t vfio_irq_count(const struct vfio_device *d, uint32_t index);
int32_t vfio_irq_set(struct vfio_device *d, uintptr_t arg);
void vfio_irq_disable(struct vfio_device *d);
void vfio_irq_restore(struct vfio_device *d);

/* igd.c */
int vfio_igd_init(struct vfio_device *d);
void vfio_igd_free(struct vfio_device *d);
uint32_t vfio_igd_regions(const struct vfio_device *d);
uint32_t vfio_igd_region_size(const struct vfio_device *d, uint32_t index);
int32_t vfio_igd_read(struct vfio_device *d, uint32_t index, uint32_t pos, uint8_t *buf,
                      uint32_t count);

/*
 * hw.c: what VFIO needs of the rest of the kernel beyond the interfaces it
 * includes, which a host test gives its own versions of.
 */

/** Wait `ms` milliseconds at least: a process sleeps, the boot spins. */
void vfio_hw_msleep(unsigned int ms);

/** The calling process's address space, or NULL. */
struct mm *vfio_hw_mm(void);

/** A new descriptor of the caller's, read-write and close-on-exec, of type
 * FD_TYPE_ANON with the operations `ops` and the object `priv`: the
 * descriptor, or -errno. */
int32_t vfio_hw_fd_new(const struct file_ops *ops, void *priv);

/** The object of the caller's descriptor `fd` when its operations are
 * `ops`, or NULL. */
void *vfio_hw_fd_object(int fd, const struct file_ops *ops);

/** Map `size` bytes of physical memory at `phys` for reading: the kernel's
 * address, or 0. */
uintptr_t vfio_hw_map(uint64_t phys, uint32_t size);
void vfio_hw_unmap(uintptr_t va, uint32_t size);

/** The boolean module parameter `key` as the boot line gives it, false
 * when it does not, read as Linux's param_set_bool reads it. */
int vfio_hw_param_bool(const char *key);

#endif /* _VFIO_H_ */
