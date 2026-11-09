# sys/drm/pc.mk: the display devices' files that reach PC-only code, for
# sys/arch/x86/sources.mk: fbdrm, which reads the loader's framebuffer from
# the boot information; bochs and the console's client, which use the
# framebuffer console; and the PC's probes. One file to a line.

DRM_PC_C_SRC = sys/drm/bochs.c \
    sys/drm/drm_client.c \
    sys/drm/drm_pc.c \
    sys/drm/fbdrm.c
