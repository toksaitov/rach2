# mk/gnu-image.mk: the GNU system's root (vendor/gnu/Readme.md), the os-real
# system of krffs/manifests/os-real-gnu.manifest with the GNU layer installed
# over it by vendor/gnu/install_root.sh, as an ext2 volume the kernel boots
# with rach2.root=ext2. It is not part of `all`. 4096 MiB: the system takes
# 1.2 GiB of it, and the rest is room for what the machine compiles and
# fetches. The root has the packages of the agents layer that GNU_AGENTS
# names (mk/gnu-agents.mk); os-real-gnu-agents.ext2, the root of `tests/run
# agents`, has every package the layer has.
#
# On an x86-64 Linux host the image is built where the GNU layer is. Its
# prerequisites are those of os-real's ext2 image, less Dropbear, with the
# overlay, the list with the CA bundle's digest, the five stamps of the
# layer, and the list and the stamps of the agents' packages it has. On any
# other host, or wherever GNU_TOOLING is set to a word, the layer and the
# image are built in etude's tooling machine by vendor/gnu/tooling_image.sh,
# on a disk that keeps the layer's build tree between its jobs. There the
# stamps are on that disk, where only the machine's make reads them, so the
# image depends on what they depend on instead: the sources of each stamp,
# ports.link, and the recipe of each agents' package it has. The host builds
# the products the manifest stages, etude, which runs the machine, and the
# digest tool that checks the image the machine sends back.
GNU_TOOLING ?= $(if $(filter Linux_x86_64,$(shell uname -s)_$(shell uname -m)),,yes)
OS_REAL_GNU_EXT2 = $(IMAGES_DIR)/os-real-gnu.ext2
OS_REAL_GNU_AGENTS_EXT2 = $(IMAGES_DIR)/os-real-gnu-agents.ext2
GNU_TEST_EXT2 = $(IMAGES_DIR)/gnu-test.ext2
# Stage the exact kernel profiles beside the GNU test products. These file
# rules share the ordinary component producers; no kernel is rebuilt here.
$(GNU_TEST_EXT2): build/tests/host-suite/scripts-assets.tar $(KERNEL) $(METAL_KERNEL)
GNU_IMAGE_STAMPS = $(GNU_PORTS_STAMP) $(GNU_NATIVE_STAMP) $(GNU_GDB_STAMP) \
                   $(GNU_JS_STAMP) $(GNU_CHROMIUM_STAMP)
GNU_IMAGE_DEPS = $(DEMO_CREDS) krffs/manifests/os-real-gnu.manifest $(MANIFEST_DEPS_os_real_gnu) \
                 tools/image/mkext2.sh vendor/gnu/install_root.sh vendor/gnu/fetch.sh \
                 tests/system/fakeapi/native/wsclient.c tests/cases/host/products.mk \
                 vendor/gnu/sources-extra.txt $(GNU_IMAGE_STAMPS) $(IMAGE_TOOLS) $(MAN_ALL)
# LOGINS names a directory of logins laid out from the machine's root, as
# tools/dev/machine_logins.sh pull writes one, such as
# ~/Desktop/rach2-agent-creds: os-real-gnu.ext2, the machine's root, and no
# test image, has its root/ staged in /root by tools/image/stage_logins.sh.
# The stamp holds that script's digest of what it stages, `none` without
# LOGINS, and is rewritten only when it changes, so that a root staged with
# other logins, or with none, is made again.
LOGINS ?=
GNU_LOGINS_STAMP = $(IMAGES_DIR)/os-real-gnu.logins
GNU_STAGE_LOGINS = $(if $(LOGINS),sh tools/image/stage_logins.sh $@ "$(LOGINS)")
$(GNU_LOGINS_STAMP): FORCE
	@mkdir -p $(@D)
	@sh tools/image/stage_logins.sh --digest "$(LOGINS)" > $@.new
	@if cmp -s $@.new $@; then rm -f $@.new; else mv $@.new $@; fi
GNU_IMAGE_ORDER = kbd preflight krffs rach2 utils check-busybox check-tcc check-pdpmake check-setid \
                  check-e2fsck tools/etude/guest ports/doom term unifont

# A layer built in this copy is removed before any of its stamps is built
# (mk/gnu.mk); every fragment has added its stamps by now.
ifneq ($(GNU_IN_COPY),)
$(GNU_STEP_STAMPS) $(GNU_AGENTS_ALL_STAMPS): gnu-in-copy
endif

.PHONY: os-real-gnu-ext2 os-real-gnu-agents-ext2
os-real-gnu-ext2: $(OS_REAL_GNU_EXT2)
os-real-gnu-agents-ext2: $(OS_REAL_GNU_AGENTS_EXT2)

ifeq ($(GNU_TOOLING),)
$(GNU_TEST_EXT2): $(GNU_IMAGE_DEPS) $(GNU_AGENTS_LIST) $(GNU_AGENTS_STAMPS) \
    $(GNU_SANITIZERS_STAMP) $(GNU_HOST_PRODUCTS_STAMP) $(GNU_FSTOOLS_STAMP) \
    $(if $(filter x86_64,$(ARCH)),$(GNU_HOST_CHECKS_STAMP)) | $(GNU_IMAGE_ORDER)
	RACH2_ARCH=$(ARCH) GNU_AGENTS="$(GNU_AGENTS_ON)" GNU_TEST_PRODUCTS=1 \
	    sh tools/image/mkext2.sh --overlay vendor/gnu/install_root.sh \
	    krffs/manifests/os-real-gnu.manifest $@ 12288

$(OS_REAL_GNU_EXT2): $(GNU_IMAGE_DEPS) $(GNU_AGENTS_LIST) $(GNU_AGENTS_STAMPS) $(GNU_LOGINS_STAMP) \
                     tools/image/stage_logins.sh | $(GNU_IMAGE_ORDER)
	RACH2_ARCH=$(ARCH) GNU_AGENTS="$(GNU_AGENTS_ON)" sh tools/image/mkext2.sh \
	    --overlay vendor/gnu/install_root.sh krffs/manifests/os-real-gnu.manifest $@ 4096
	$(GNU_STAGE_LOGINS)

$(OS_REAL_GNU_AGENTS_EXT2): $(GNU_IMAGE_DEPS) $(GNU_AGENTS_ALL_LIST) $(GNU_AGENTS_ALL_STAMPS) | $(GNU_IMAGE_ORDER)
	RACH2_ARCH=$(ARCH) GNU_AGENTS="$(GNU_AGENTS_ALL)" sh tools/image/mkext2.sh \
	    --overlay vendor/gnu/install_root.sh krffs/manifests/os-real-gnu.manifest $@ 4096
else
GNU_TOOLING_DEPS = $(filter-out $(GNU_IMAGE_STAMPS),$(GNU_IMAGE_DEPS)) \
                   $(sort $(GNU_TOOLCHAIN_SOURCES) $(GNU_PORTS_SOURCES) $(GNU_NATIVE_SOURCES) \
                          $(GNU_GDB_SOURCES) $(GNU_JS_SOURCES) $(GNU_CHROMIUM_SOURCES)) \
                   $(GNU_PORTS_LINK) vendor/gnu/tooling_image.sh vendor/gnu/tooling_job.sh \
                   tools/setup/elfwrap.sh
GNU_TOOLING_ORDER = $(GNU_IMAGE_ORDER) $(ETUDE) $(SHA256TOOL)
GNU_AGENTS_RECIPES = $(patsubst %,vendor/gnu/agents/packages/%.sh,$(filter $(GNU_AGENTS_ALL),$(GNU_AGENTS_ON)))
GNU_AGENTS_ALL_RECIPES = $(patsubst %,vendor/gnu/agents/packages/%.sh,$(GNU_AGENTS_ALL))

$(GNU_TEST_EXT2): $(GNU_TOOLING_DEPS) $(GNU_AGENTS_LIST) $(GNU_AGENTS_RECIPES) \
    $(GNU_SANITIZER_SOURCES) $(GNU_HOST_PRODUCTS_SOURCES) $(GNU_FSTOOLS_SOURCES) \
    $(if $(filter x86_64,$(ARCH)),$(GNU_HOST_CHECKS_SOURCES) \
        build/tests/host-suite/tools/exfat_external/exfat_external.img) \
    $(if $(GNU_AGENTS_RECIPES),$(GNU_AGENTS_SOURCES)) | $(GNU_TOOLING_ORDER)
	RACH2_ARCH=$(ARCH) GNU_AGENTS="$(GNU_AGENTS_ON)" GNU_LINK="$(GNU_LINK)" \
	    GNU_TEST_PRODUCTS=1 MUSL_CRYPT_SRC=$(MUSL_CRYPT_SRC) \
	    sh vendor/gnu/tooling_image.sh $@ 12288

$(OS_REAL_GNU_EXT2): $(GNU_TOOLING_DEPS) $(GNU_AGENTS_LIST) $(GNU_AGENTS_RECIPES) \
                     $(if $(GNU_AGENTS_RECIPES),$(GNU_AGENTS_SOURCES)) $(GNU_LOGINS_STAMP) \
                     tools/image/stage_logins.sh | $(GNU_TOOLING_ORDER)
	RACH2_ARCH=$(ARCH) GNU_AGENTS="$(GNU_AGENTS_ON)" GNU_LINK="$(GNU_LINK)" \
	    MUSL_CRYPT_SRC=$(MUSL_CRYPT_SRC) sh vendor/gnu/tooling_image.sh $@ 4096
	$(GNU_STAGE_LOGINS)

$(OS_REAL_GNU_AGENTS_EXT2): $(GNU_TOOLING_DEPS) $(GNU_AGENTS_ALL_LIST) $(GNU_AGENTS_ALL_RECIPES) \
                            $(GNU_AGENTS_SOURCES) | $(GNU_TOOLING_ORDER)
	RACH2_ARCH=$(ARCH) GNU_AGENTS="$(GNU_AGENTS_ALL)" GNU_LINK="$(GNU_LINK)" \
	    MUSL_CRYPT_SRC=$(MUSL_CRYPT_SRC) sh vendor/gnu/tooling_image.sh $@ 4096
endif

# `make machine`: everything an installed machine is given, in one target, so
# that no part of it is left from an older build: the kernel, the recovery
# kernel with its checks, the recovery image with this tree's key in it, and
# the GNU root. `all` builds neither recovery part, so a machine given `all`'s
# products could have a recovery system older than its kernel. The key is
# minted by a make of its own, because make reads the recovery image's name,
# which says whether it has a key, when it starts. The GNU root is built as
# the rules above say, with LOGINS staged in it when given.
.PHONY: machine
machine:
	$(MAKE) --no-print-directory recovery-key
	$(MAKE) --no-print-directory rach2 rach2-recovery rach2-recovery-image os-real-gnu-ext2
