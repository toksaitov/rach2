# sys/dmabuf/sources.mk: the sharing of buffers and fences between devices
# and processes, for rach2/sources.mk: dma-bufs, fences, sync files,
# udmabuf and sw_sync, which every architecture compiles.

DMABUF_C_SRC = sys/dmabuf/dmabuf.c \
    sys/dmabuf/fence.c \
    sys/dmabuf/sw_sync.c \
    sys/dmabuf/sync_file.c \
    sys/dmabuf/udmabuf.c
