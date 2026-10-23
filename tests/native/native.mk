# native.mk: immutable files for the collection on an already booted Rach2.
.PHONY: native-controller native-assets
native-controller: $(LIBC_STAMP)
	+$(MAKE) --no-print-directory -C tests/native
$(ARCH_BUILD)/tests/native/native-collection: | native-controller
	@test -x $@
native-assets: tests musl-programs tools/etude/guest native-controller krffs \
    build/tests/host/mkexfatfixture $(IMAGES_DIR)/ext2-fixture.disk \
    build/tests/system/loopdev/fat.img build/tests/system/loopdev/ext2.img \
    build/tests/system/loopdev/krffs.img build/tests/system/krffsvol/volume.krffs \
    build/tests/system/krffsvol/badchain.krffs build/tests/system/krffsvol/badmagic.krffs \
    build/tests/cases/gnu/sed.tar.xz krffs/authorized_keys krffs/shadow
	python3 tests/native/prepare.py --architecture $(ARCH) --output $(ARCH_BUILD)/tests/native/prepared
