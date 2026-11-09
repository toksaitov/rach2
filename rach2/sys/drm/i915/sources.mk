# sys/drm/i915/sources.mk: the Intel graphics driver's files, for
# sys/arch/x86/sources.mk, since the driver is PC-only. One file to a line,
# each task adding its own.

I915_C_SRC = sys/drm/i915/i915_hw.c \
    sys/drm/i915/i915_pci.c
