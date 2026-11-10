/*
 * vfio_abi.h: the part of Linux's VFIO interface (include/uapi/linux/vfio.h)
 * that Rach2 gives: the container, the group, a PCI device's regions and
 * interrupts, and the Type1 IOMMU. The numbers and layouts are Linux's.
 */

#ifndef _VFIO_ABI_H_
#define _VFIO_ABI_H_

#include "../lib/kint.h"

#define VFIO_API_VERSION 0

/* The extensions VFIO_CHECK_EXTENSION asks about. */
#define VFIO_TYPE1_IOMMU         1
#define VFIO_SPAPR_TCE_IOMMU     2
#define VFIO_TYPE1v2_IOMMU       3
#define VFIO_DMA_CC_IOMMU        4
#define VFIO_EEH                 5
#define VFIO_TYPE1_NESTING_IOMMU 6
#define VFIO_SPAPR_TCE_v2_IOMMU  7
#define VFIO_NOIOMMU_IOMMU       8
#define VFIO_UNMAP_ALL           9
#define VFIO_UPDATE_VADDR        10

/* _IO(';', 100 + n): no direction and no size in the number. */
#define VFIO_IO(n) (0x3B00u | (100u + (n)))

#define VFIO_GET_API_VERSION               VFIO_IO(0)
#define VFIO_CHECK_EXTENSION               VFIO_IO(1)
#define VFIO_SET_IOMMU                     VFIO_IO(2)
#define VFIO_GROUP_GET_STATUS              VFIO_IO(3)
#define VFIO_GROUP_SET_CONTAINER           VFIO_IO(4)
#define VFIO_GROUP_UNSET_CONTAINER         VFIO_IO(5)
#define VFIO_GROUP_GET_DEVICE_FD           VFIO_IO(6)
#define VFIO_DEVICE_GET_INFO               VFIO_IO(7)
#define VFIO_DEVICE_GET_REGION_INFO        VFIO_IO(8)
#define VFIO_DEVICE_GET_IRQ_INFO           VFIO_IO(9)
#define VFIO_DEVICE_SET_IRQS               VFIO_IO(10)
#define VFIO_DEVICE_RESET                  VFIO_IO(11)
#define VFIO_DEVICE_GET_PCI_HOT_RESET_INFO VFIO_IO(12)
#define VFIO_DEVICE_PCI_HOT_RESET          VFIO_IO(13)
#define VFIO_DEVICE_IOEVENTFD              VFIO_IO(16)
#define VFIO_DEVICE_FEATURE                VFIO_IO(17)
#define VFIO_IOMMU_GET_INFO                VFIO_IO(12)
#define VFIO_IOMMU_MAP_DMA                 VFIO_IO(13)
#define VFIO_IOMMU_UNMAP_DMA               VFIO_IO(14)
#define VFIO_IOMMU_DIRTY_PAGES             VFIO_IO(17)

struct vfio_group_status {
    uint32_t argsz;
    uint32_t flags;
};
#define VFIO_GROUP_FLAGS_VIABLE        (1u << 0)
#define VFIO_GROUP_FLAGS_CONTAINER_SET (1u << 1)

/* The header of each capability of a chain an info structure carries;
 * `next` is the next one's offset from the start of the structure, or 0. */
struct vfio_info_cap_header {
    uint16_t id;
    uint16_t version;
    uint32_t next;
};

struct vfio_device_info {
    uint32_t argsz;
    uint32_t flags;
    uint32_t num_regions;
    uint32_t num_irqs;
    uint32_t cap_offset;
    uint32_t pad;
};
#define VFIO_DEVICE_FLAGS_RESET (1u << 0)
#define VFIO_DEVICE_FLAGS_PCI   (1u << 1)
#define VFIO_DEVICE_FLAGS_CAPS  (1u << 7)

struct vfio_region_info {
    uint32_t argsz;
    uint32_t flags;
    uint32_t index;
    uint32_t cap_offset;
    uint64_t size;
    uint64_t offset;
};
#define VFIO_REGION_INFO_FLAG_READ  (1u << 0)
#define VFIO_REGION_INFO_FLAG_WRITE (1u << 1)
#define VFIO_REGION_INFO_FLAG_MMAP  (1u << 2)
#define VFIO_REGION_INFO_FLAG_CAPS  (1u << 3)

#define VFIO_REGION_INFO_CAP_TYPE          2
#define VFIO_REGION_INFO_CAP_MSIX_MAPPABLE 3

struct vfio_region_info_cap_type {
    struct vfio_info_cap_header header;
    uint32_t type;
    uint32_t subtype;
};
#define VFIO_REGION_TYPE_PCI_VENDOR_TYPE       (1u << 31)
#define VFIO_REGION_SUBTYPE_INTEL_IGD_OPREGION 1
#define VFIO_REGION_SUBTYPE_INTEL_IGD_HOST_CFG 2
#define VFIO_REGION_SUBTYPE_INTEL_IGD_LPC_CFG  3

struct vfio_irq_info {
    uint32_t argsz;
    uint32_t flags;
    uint32_t index;
    uint32_t count;
};
#define VFIO_IRQ_INFO_EVENTFD    (1u << 0)
#define VFIO_IRQ_INFO_MASKABLE   (1u << 1)
#define VFIO_IRQ_INFO_AUTOMASKED (1u << 2)
#define VFIO_IRQ_INFO_NORESIZE   (1u << 3)

struct vfio_irq_set {
    uint32_t argsz;
    uint32_t flags;
    uint32_t index;
    uint32_t start;
    uint32_t count;
    /* then `count` bools or eventfd descriptors */
};
#define VFIO_IRQ_SET_DATA_NONE        (1u << 0)
#define VFIO_IRQ_SET_DATA_BOOL        (1u << 1)
#define VFIO_IRQ_SET_DATA_EVENTFD     (1u << 2)
#define VFIO_IRQ_SET_ACTION_MASK      (1u << 3)
#define VFIO_IRQ_SET_ACTION_UNMASK    (1u << 4)
#define VFIO_IRQ_SET_ACTION_TRIGGER   (1u << 5)
#define VFIO_IRQ_SET_DATA_TYPE_MASK   0x7u
#define VFIO_IRQ_SET_ACTION_TYPE_MASK 0x38u

/* A PCI device's regions and interrupts, by index. */
#define VFIO_PCI_BAR0_REGION_INDEX   0
#define VFIO_PCI_BAR5_REGION_INDEX   5
#define VFIO_PCI_ROM_REGION_INDEX    6
#define VFIO_PCI_CONFIG_REGION_INDEX 7
#define VFIO_PCI_VGA_REGION_INDEX    8
#define VFIO_PCI_NUM_REGIONS         9

#define VFIO_PCI_INTX_IRQ_INDEX 0
#define VFIO_PCI_MSI_IRQ_INDEX  1
#define VFIO_PCI_MSIX_IRQ_INDEX 2
#define VFIO_PCI_ERR_IRQ_INDEX  3
#define VFIO_PCI_REQ_IRQ_INDEX  4
#define VFIO_PCI_NUM_IRQS       5

struct vfio_iommu_type1_info {
    uint32_t argsz;
    uint32_t flags;
    uint64_t iova_pgsizes;
    uint32_t cap_offset;
    uint32_t pad;
};
#define VFIO_IOMMU_INFO_PGSIZES (1u << 0)
#define VFIO_IOMMU_INFO_CAPS    (1u << 1)

#define VFIO_IOMMU_TYPE1_INFO_CAP_IOVA_RANGE 1
#define VFIO_IOMMU_TYPE1_INFO_DMA_AVAIL      3

struct vfio_iova_range {
    uint64_t start;
    uint64_t end;
};

struct vfio_iommu_type1_info_cap_iova_range {
    struct vfio_info_cap_header header;
    uint32_t nr_iovas;
    uint32_t reserved;
    /* then `nr_iovas` struct vfio_iova_range */
};

struct vfio_iommu_type1_info_dma_avail {
    struct vfio_info_cap_header header;
    uint32_t avail;
};

struct vfio_iommu_type1_dma_map {
    uint32_t argsz;
    uint32_t flags;
    uint64_t vaddr;
    uint64_t iova;
    uint64_t size;
};
#define VFIO_DMA_MAP_FLAG_READ  (1u << 0)
#define VFIO_DMA_MAP_FLAG_WRITE (1u << 1)
#define VFIO_DMA_MAP_FLAG_VADDR (1u << 2)

struct vfio_iommu_type1_dma_unmap {
    uint32_t argsz;
    uint32_t flags;
    uint64_t iova;
    uint64_t size;
};
#define VFIO_DMA_UNMAP_FLAG_GET_DIRTY_BITMAP (1u << 0)
#define VFIO_DMA_UNMAP_FLAG_ALL              (1u << 1)
#define VFIO_DMA_UNMAP_FLAG_VADDR            (1u << 2)

/* A region's place in a device's descriptor: its index above bit 40, as
 * Linux's VFIO_PCI_OFFSET_SHIFT has it. As a page offset of mmap that is
 * the index above bit 28, which the 32-bit page offset holds for indexes
 * below 16. */
#define VFIO_PCI_OFFSET_SHIFT 40
#define VFIO_PCI_PGOFF_SHIFT  28

#endif /* _VFIO_ABI_H_ */
