# native.mk: immutable files for the collection on an already booted Rach2.
include tests/native/sources.mk
.PHONY: native-controller native-assets
native-controller: $(LIBC_STAMP)
	+$(MAKE) --no-print-directory -C tests/native
$(ARCH_BUILD)/tests/native/etude: | native-controller
	@test -x $@
native-assets: tests musl-programs utils ish check-dropbearmulti tools/etude/guest native-controller krffs \
    build/tests/host/mkexfatfixture $(IMAGES_DIR)/ext2-fixture.disk \
    build/tests/system/loopdev/fat.img build/tests/system/loopdev/ext2.img \
    build/tests/system/loopdev/krffs.img build/tests/system/krffsvol/volume.krffs \
    build/tests/system/krffsvol/badchain.krffs build/tests/system/krffsvol/badmagic.krffs \
    build/tests/cases/gnu/sed.tar.xz krffs/authorized_keys krffs/shadow
	python3 tests/native/prepare.py --architecture $(ARCH) --output $(ARCH_BUILD)/tests/native/prepared

# Deployment is built on the owner's Mac and is never an execution fixture.
build/tools/dev/native-deploy: $(NATIVE_DEPLOY_SRC) tests/native/deploy.h $(ETUDE_HDRS)
	@mkdir -p $(dir $@)
	$(ETUDE_LINK) -std=c99 -D_DEFAULT_SOURCE -Itools/etude/include -Itools/etude \
	    -Itools/etude/posix -Iutils

build/tools/dev/native-hostpeer: tests/system/runner/hostpeer.c
	@mkdir -p $(dir $@)
	$(ETUDE_LINK) -std=c99 -D_DEFAULT_SOURCE
