# sys/drm/i915/sources.mk: the Intel graphics driver's files, for
# sys/arch/x86/sources.mk, since the driver is PC-only. One file to a line,
# each task adding its own.

I915_C_SRC = sys/drm/i915/i915_hw.c \
    sys/drm/i915/i915_pci.c \
    sys/drm/i915/i915_uncore.c \
    sys/drm/i915/i915_power.c \
    sys/drm/i915/i915_stolen.c \
    sys/drm/i915/i915_gtt.c \
    sys/drm/i915/i915_irq.c \
    sys/drm/i915/i915_ring.c \
    sys/drm/i915/i915_ppgtt.c \
    sys/drm/i915/i915_firstlight.c \
    sys/drm/i915/i915_rc6.c \
    sys/drm/i915/i915_rps.c \
    sys/drm/i915/intel_opregion.c \
    sys/drm/i915/intel_bios.c \
    sys/drm/i915/i915_fake.c \
    sys/drm/i915/i915_aperture.c \
    sys/drm/i915/i915_fence.c \
    sys/drm/i915/i915_gem.c \
    sys/drm/i915/i915_vma.c \
    sys/drm/i915/i915_gem_evict.c \
    sys/drm/i915/i915_gem_shrinker.c \
    sys/drm/i915/i915_gem_userptr.c \
    sys/drm/i915/i915_gem_tiling.c \
    sys/drm/i915/i915_gem_mman.c \
    sys/drm/i915/i915_gem_prime.c \
    sys/drm/i915/i915_gem_context.c \
    sys/drm/i915/i915_request.c \
    sys/drm/i915/i915_getparam.c \
    sys/drm/i915/i915_reg_read.c \
    sys/drm/i915/intel_display.c \
    sys/drm/i915/intel_plane.c \
    sys/drm/i915/intel_cursor.c \
    sys/drm/i915/intel_vblank.c \
    sys/drm/i915/intel_backlight.c \
    sys/drm/i915/i915_display_debugfs.c \
    sys/drm/i915/i915_gem_execbuffer.c \
    sys/drm/i915/i915_cmd_parser.c \
    sys/drm/i915/intel_cdclk.c \
    sys/drm/i915/intel_pps.c \
    sys/drm/i915/intel_dp_aux.c \
    sys/drm/i915/intel_gmbus.c \
    sys/drm/i915/intel_dpll.c \
    sys/drm/i915/intel_hdmi.c \
    sys/drm/i915/intel_hotplug.c \
    sys/drm/i915/intel_ddi.c \
    sys/drm/i915/intel_dp_link_training.c \
    sys/drm/i915/intel_dp.c \
    sys/drm/i915/intel_panel.c \
    sys/drm/i915/intel_pfit.c \
    sys/drm/i915/intel_wm.c \
    sys/drm/i915/i915_hangcheck.c \
    sys/drm/i915/i915_reset.c \
    sys/drm/i915/i915_gpu_error.c \
    sys/drm/i915/i915_debugfs.c
