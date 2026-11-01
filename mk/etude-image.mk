# The small Rach2 helper used by native collection image checks.
MTOOLS_STAMP = $(ARCH_VENDOR)/mtools/.stamp
MTOOLS_STAGE = $(VENDOR_STAGE)/mtools
MTOOLS_BIN = $(MTOOLS_STAGE)/mtools
MTOOLS_TARBALL = vendor/mtools/mtools-4.0.49.tar.gz
ETUDE_IMAGE = $(IMAGES_DIR)/etude-image.krffs
ETUDE_IMAGE_KERNEL = $(ARCH_BUILD)/rach2-recovery/kernel
ETUDE_IMAGE_MIB = 16
ETUDE_IMAGE_FREE_KIB = 4096

.PHONY: mtools check-mtools etude-image etude-image-force
mtools: $(MTOOLS_STAMP)
etude-image: $(ETUDE_IMAGE)
etude-image-force:
$(MTOOLS_STAMP): vendor/mtools/build_mtools.sh vendor/mtools/offset64.patch \
    vendor/mtools/block-geometry.patch \
    $(MTOOLS_TARBALL) $(VENDOR_COMMON) \
                 $(LIBC_STAMP) | $(SHA256TOOL) $(ETUDE)
	@rm -f $@
	+@$(STEP) -n mtools -t $@ -c $(CROSS_PREFIX)gcc -c cc $(STEP_INPUTS) \
	    -o $(MTOOLS_STAGE) -- sh -c 'RACH2_ARCH=$(ARCH) sh vendor/mtools/build_mtools.sh'
	@test -s $(MTOOLS_BIN)
$(MTOOLS_BIN): $(MTOOLS_STAMP)
check-mtools: $(MTOOLS_BIN) $(ELFCHECK)
	$(ELFCHECK) --arch $(ARCH) $(MTOOLS_BIN)
	@test $$(wc -c < $(MTOOLS_BIN)) -le 1048576

# The direct Multiboot module must leave the boot allocator below 64 MiB.
# Check cached images too, but restage only changed inputs or an old size.
$(ETUDE_IMAGE): krffs/manifests/etude-image.manifest mk/etude-image.mk \
                 etude-image-force $(MTOOLS_BIN) $(DEBUGFS_BIN) \
                 $(ARCH_BUILD)/tests/cases/host/image-channel-fixture \
                 $(ARCH_BUILD)/tests/cases/machine/installer-repair-fixture \
                 $(E2FSCK_BIN) $(MKE2FS_BIN) $(DUMPE2FS_BIN) $(TUNE2FS_BIN) $(RESIZE2FS_BIN) \
                 $(BUSYBOX_BIN) $(ARCH_BUILD)/tools/etude/guest/etude-guest $(IMAGE_TOOLS) \
                 | preflight krffs tools/etude/guest check-mtools check-e2fsck check-busybox \
                   $(ELFCHECK) $(ETUDE_IMAGE_KERNEL)
	@if [ -n "$(filter-out etude-image-force,$?)" ] || [ ! -f "$@" ] || \
	    [ "$$(wc -c < "$@")" -ne $$(( $(ETUDE_IMAGE_MIB) * 1048576 )) ]; then \
	    $(MKIMAGE) krffs/manifests/etude-image.manifest $@ \
	        $(ETUDE_IMAGE_MIB) $(ETUDE_IMAGE_FREE_KIB); \
	fi
	@$(ELFCHECK) --kernel --initrd $@ $(ETUDE_IMAGE_KERNEL) \
	    || { rm -f $@; exit 1; }

$(ARCH_BUILD)/tests/cases/host/image-channel-fixture: tests/cases/host/image_channel_fixture.c \
    $(LIBC_STAMP)
	@mkdir -p $(@D)
	RACH2_ARCH=$(ARCH) vendor/musl/muslgcc -std=c99 -Os -static $< -o $@

# The loop-mount acceptance fixture is built once and copied per attempt.
build/tests/cases/machine/metal/loop.krffs: build/tests/system/loopdev/krffs.img
	@mkdir -p $(@D)
	cp $< $@

$(ARCH_BUILD)/tests/cases/machine/installer-repair-fixture: \
    tests/cases/machine/installer_repair_fixture.c \
    tests/cases/machine/installer_repair_fixture_edits.c \
    tests/cases/machine/installer_repair_fixture.h $(LIBC_STAMP)
	@mkdir -p $(@D)
	RACH2_ARCH=$(ARCH) vendor/musl/muslgcc -std=c99 -Os -static \
	    tests/cases/machine/installer_repair_fixture.c \
	    tests/cases/machine/installer_repair_fixture_edits.c -o $@
