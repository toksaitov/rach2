# sys/virtio/sources.mk: the virtio transport and its queues, for
# rach2/sources.mk, which every architecture compiles: the core that every
# transport shares and the PCI transport. One file to a line.

VIRTIO_C_SRC = sys/virtio/virtqueue.c \
    sys/virtio/virtio_pci.c
