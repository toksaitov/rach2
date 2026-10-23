# scripts.mk: one native scenario and immutable sources for host.list.
HOST_SCRIPT_CASE_SRC = tests/cases/host/host_scripts_scenario.c \
    tests/cases/host/host_scripts.c tests/cases/host/host_build.c \
    tests/cases/host/host_image.c tests/cases/host/host_image_accounts.c \
    tests/cases/host/host_image_krffs.c tests/cases/host/host_image_overlay.c \
    tests/cases/host/host_image_tooling.c tests/cases/host/host_image_collection.c \
    tests/cases/machine/netcase.c
build/tests/cases/host/host_scripts_scenario: $(HOST_SCRIPT_CASE_SRC) \
    tests/cases/host/host_scripts.h tests/cases/host/host_build.h tests/cases/host/host_image.h \
    tests/cases/machine/netcase.h $(ETUDE_CASE_LIB_SRC) $(ETUDE_CASE_LIB_HDRS)
	@mkdir -p $(@D)
	$(ETUDE_LINK) -std=c99 -D_DEFAULT_SOURCE -Itools/etude/include -Itools/etude \
	    -Itools/etude/posix -Itools/etude/guest -Iutils

HOST_SCRIPT_ASSETS = $(wildcard $(shell git ls-files tools tests mk Makefile \
    utils/sha256.c utils/sha256.h rach2/run.sh rach2/run64.sh rach2/run-gdb.sh \
    rach2/debug.sh rach2/qemu_devices.sh vendor/gnu vendor/tcc/asm-test vendor/tcc/ld-test))
build/tests/host-suite/scripts-assets.tar: $(HOST_SCRIPT_ASSETS)
	@mkdir -p $(@D)
	COPYFILE_DISABLE=1 tar --format=ustar -cf $@ $(HOST_SCRIPT_ASSETS)

build/tests/host-suite/sources/%: %
	@mkdir -p $(@D)
	cp $< $@

# The products helper executes both x86 ABIs even in an i686-only collection.
ifneq ($(ARCH),x86_64)
.PHONY: prepare-host-products-x86_64
prepare-host-products-x86_64:
	+$(MAKE) --no-print-directory ARCH=x86_64 rach2 rach2-recovery \
	    build/x86_64/images/gnu-test.disk build/x86_64/images/etude-image.krffs
build/x86_64/rach2/kernel build/x86_64/images/gnu-test.disk \
build/x86_64/rach2-recovery/kernel build/x86_64/images/etude-image.krffs: | prepare-host-products-x86_64
	@test -s $@
endif
