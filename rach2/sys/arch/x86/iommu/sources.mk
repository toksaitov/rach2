# sys/arch/x86/iommu/sources.mk: Intel's VT-d, for sys/arch/x86_64/sources.mk:
# the DMAR table, the units and their domains, interrupt remapping, the
# fault reports, and what the driver needs of the machine. Only x86-64
# compiles it, since only its VFIO and KVM give devices to VMs.

IOMMU_C_SRC = sys/arch/x86/iommu/dmar.c \
    sys/arch/x86/iommu/vtd.c \
    sys/arch/x86/iommu/intremap.c \
    sys/arch/x86/iommu/fault.c \
    sys/arch/x86/iommu/hw.c
