# The small Rach2 helper used by native collection image checks.
MTOOLS_STAMP = $(ARCH_VENDOR)/mtools/.stamp
MTOOLS_STAGE = $(VENDOR_STAGE)/mtools
MTOOLS_BIN = $(MTOOLS_STAGE)/mtools
MTOOLS_TARBALL = vendor/mtools/mtools-4.0.49.tar.gz
ETUDE_IMAGE = $(IMAGES_DIR)/etude-image.krffs

.PHONY: mtools check-mtools etude-image
mtools: $(MTOOLS_STAMP)
etude-image: $(ETUDE_IMAGE)
$(MTOOLS_STAMP): vendor/mtools/build_mtools.sh $(MTOOLS_TARBALL) $(VENDOR_COMMON) \
                 $(LIBC_STAMP) | $(SHA256TOOL) $(ETUDE)
	@rm -f $@
	+@$(STEP) -n mtools -t $@ -c $(CROSS_PREFIX)gcc -c cc $(STEP_INPUTS) \
	    -o $(MTOOLS_STAGE) -- sh -c 'RACH2_ARCH=$(ARCH) sh vendor/mtools/build_mtools.sh'
	@test -s $(MTOOLS_BIN)
$(MTOOLS_BIN): $(MTOOLS_STAMP)
check-mtools: $(MTOOLS_BIN) $(ELFCHECK)
	$(ELFCHECK) --arch $(ARCH) $(MTOOLS_BIN)
	@test $$(wc -c < $(MTOOLS_BIN)) -le 1048576

$(ETUDE_IMAGE): krffs/manifests/etude-image.manifest $(MTOOLS_BIN) $(DEBUGFS_BIN) \
                 $(E2FSCK_BIN) $(MKE2FS_BIN) $(DUMPE2FS_BIN) $(TUNE2FS_BIN) $(RESIZE2FS_BIN) \
                 $(BUSYBOX_BIN) $(ARCH_BUILD)/tools/etude/guest/etude-guest $(IMAGE_TOOLS) \
                 | preflight krffs tools/etude/guest check-mtools check-e2fsck check-busybox
	$(MKIMAGE) krffs/manifests/etude-image.manifest $@ 64

# The loop-mount acceptance fixture is built once and copied per attempt.
build/tests/cases/machine/metal/loop.krffs: build/tests/system/loopdev/krffs.img
	@mkdir -p $(@D)
	cp $< $@
