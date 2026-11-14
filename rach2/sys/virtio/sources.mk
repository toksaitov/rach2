# sys/virtio/sources.mk: the virtio transport and its queues, for
# rach2/sources.mk, which every architecture compiles: the core that every
# transport shares, the virtio bus in /sys, the PCI transport, the
# transport over a region shared with a VM, and the console. One file to a
# line.

VIRTIO_C_SRC = sys/virtio/virtqueue.c \
    sys/virtio/virtio_bus.c \
    sys/virtio/virtio_pci.c \
    sys/virtio/shmem.c \
    sys/virtio/virtio_console.c
