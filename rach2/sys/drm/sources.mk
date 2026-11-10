# sys/drm/sources.mk: the display devices' files that every architecture
# compiles, for rach2/sources.mk: the DRM core, virtio-gpu, vkms and vgem. The
# drivers that reach PC-only code are sys/drm/pc.mk's. One file to a line,
# each task adding its own.

DRM_C_SRC = sys/drm/drm.c \
    sys/drm/drm_atomic.c \
    sys/drm/drm_auth.c \
    sys/drm/drm_debugfs.c \
    sys/drm/drm_edid.c \
    sys/drm/drm_gem.c \
    sys/drm/drm_legacy.c \
    sys/drm/drm_mode.c \
    sys/drm/drm_prime.c \
    sys/drm/drm_syncobj.c \
    sys/drm/drm_sysfs.c \
    sys/drm/drm_vblank.c \
    sys/drm/virtio_gpu.c \
    sys/drm/virtio_gpu_3d.c \
    sys/drm/virtio_gpu_submit.c \
    sys/drm/virtio_gpu_fence.c \
    sys/drm/virtio_gpu_blob.c \
    sys/drm/vkms.c \
    sys/drm/vgem.c
