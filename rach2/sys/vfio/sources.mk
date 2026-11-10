# sys/vfio/sources.mk: VFIO, PCI devices given to programs, for
# sys/arch/x86_64/sources.mk alone, since the IOMMU it needs is x86-64's.
# One file to a line.

VFIO_C_SRC = sys/vfio/group.c \
    sys/vfio/container.c \
    sys/vfio/pci.c \
    sys/vfio/config.c \
    sys/vfio/irq.c \
    sys/vfio/igd.c \
    sys/vfio/hw.c
