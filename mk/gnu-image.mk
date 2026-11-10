# mk/gnu-image.mk: the GNU system's root (vendor/gnu/Readme.md), the os-real
# system of krffs/manifests/os-real-gnu.manifest with the GNU layer installed
# over it by vendor/gnu/install_root.sh, as an ext2 volume the kernel boots
# with rach2.root=ext2. It is not part of `all`. 16384 MiB: the desktop,
# Proton and WebGL sources share the root with the compiler and ports,
# with room for profiles and shader caches. The root has the agents GNU_AGENTS
# names (mk/gnu-agents.mk); os-real-gnu-agents.ext2, the root of `tests/run
# agents`, has every package the layer has.
#
# On an x86-64 Linux host the image is built where the GNU layer is. Its
# prerequisites are those of os-real's ext2 image, less Dropbear, with the
# overlay, the list with the CA bundle's digest, the stamps of the
# layer, and the list and the stamps of the agents' packages it has. On any
# other host, or wherever GNU_TOOLING is set to a word, the layer and the
# image are built in etude's tooling machine by vendor/gnu/tooling_image.sh,
# on a disk that keeps the layer's build tree between its jobs. There the
# stamps are on that disk, where only the machine's make reads them, so the
# image depends on what they depend on instead: the sources of each stamp,
# ports.link, and the recipe of each agents' package it has. The host builds
# the products the manifest stages, etude, which runs the machine, and the
# digest tool that checks the image the machine sends back.
#
# os-real-gnu.ext2 for x86-64 made on that host also has IGT's tests and
# VK-GL-CTS (mk/gnu-extra.mk), which install_root.sh stages when GNU_IGT and
# GNU_VK_GL_CTS are 1, and gnu-test.ext2, the volume of the disk the machine
# cases boot, which removes it (tests/system/suites.mk), has VK-GL-CTS; the
# tooling machine cannot run the builder guest, so an image it makes has
# neither.
# The x86-64 root, the gnu-test image and the agents' root have the i386
# layer (vendor/gnu/Readme.md, The i386 layer) unless GNU_I386 is 0, which
# makes the root of the pure 64-bit profile; the i686 root has none to leave
# out. GNU_I386_OPTION holds the choice, rewritten only when it changes, so
# that an image made the other way is made again.
GNU_I386 ?= 1
GNU_I386_ON = $(if $(filter x86_64,$(ARCH)),$(filter 1,$(GNU_I386)))
GNU_I386_OPTION = $(IMAGES_DIR)/gnu-i386.option
.PHONY: gnu-i386-check
$(GNU_I386_OPTION): gnu-i386-check
	@case "$(GNU_I386)" in 0 | 1) ;; *) echo "gnu: GNU_I386=$(GNU_I386): not 0 or 1" >&2; exit 1 ;; esac
	@mkdir -p $(@D)
	@echo "$(if $(GNU_I386_ON),1,0)" > $@.new
	@if cmp -s $@.new $@; then rm -f $@.new; else mv $@.new $@; fi
GNU_TOOLING ?= $(if $(filter Linux_x86_64,$(shell uname -s)_$(shell uname -m)),,yes)
OS_REAL_GNU_EXT2 = $(IMAGES_DIR)/os-real-gnu.ext2
OS_REAL_GNU_AGENTS_EXT2 = $(IMAGES_DIR)/os-real-gnu-agents.ext2
GNU_TEST_EXT2 = $(IMAGES_DIR)/gnu-test.ext2
GNU_IMAGE_STAMPS = $(GNU_PORTS_STAMP) $(GNU_NATIVE_STAMP) $(GNU_GDB_STAMP) \
                   $(GNU_JS_STAMP) $(GNU_DEBIAN_STAMP) $(GNU_PROTON_STAMP) $(GNU_WEBGL_STAMP) \
                   $(GNU_VENUS_STAMP) $(GNU_VA_STAMP) $(GNU_GAMES_STAMP) $(if $(GNU_I386_ON),$(GNU_I386_STAMP)) \
                   $(if $(filter x86_64,$(ARCH)),$(GNU_KVM_STAMP))
# The native games' replays, which install_games.sh copies into the root.
GNU_GAMES_REPLAYS = $(shell find vendor/gnu/games/replays -type f 2>/dev/null)
GNU_IMAGE_DEPS = mk/gnu-image.mk $(GNU_I386_OPTION) $(DEMO_CREDS) \
                 tools/image/mkext2.sh vendor/gnu/install_root.sh vendor/gnu/fetch.sh \
                 $(wildcard vendor/gnu/desktop/*) $(wildcard vendor/gnu/games/*.sh) $(GNU_GAMES_REPLAYS) vendor/gnu/kvm/install_kvm.sh \
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
# The phony prerequisite runs the comparison at every make.
.PHONY: gnu-logins-check
$(GNU_LOGINS_STAMP): gnu-logins-check
	@mkdir -p $(@D)
	@sh tools/image/stage_logins.sh --digest "$(LOGINS)" > $@.new
	@if cmp -s $@.new $@; then rm -f $@.new; else mv $@.new $@; fi
GNU_IMAGE_ORDER = sox_ng kbd preflight krffs rach2 utils check-busybox check-tcc check-pdpmake check-setid \
                  check-e2fsck tools/etude/guest ports/doom term unifont

# A layer built in this copy is removed before any of its stamps is built
# (mk/gnu.mk); every fragment has added its stamps by now.
ifneq ($(GNU_IN_COPY),)
$(GNU_STEP_STAMPS) $(GNU_AGENTS_ALL_STAMPS): gnu-in-copy
endif

.PHONY: gnu-image os-real-gnu-ext2 os-real-gnu-agents-ext2
gnu-image: os-real-gnu-ext2
# gnu-test.ext2 is staged from os-real-gnu.manifest less the lines of /src:
# the sources of the kernel, the utilities, ish and term, from which an
# installed machine builds its own, and which no case that boots the image
# reads. With them, every change to the kernel's sources would make the image
# again. The filtered manifest is rewritten only when it changes. It keeps
# the root's name, by which the set-id guard finds its declarations
# (tools/inspect/setid.allow), where the files of /src are optional.
GNU_ROOT_MANIFEST = krffs/manifests/os-real-gnu.manifest
GNU_TEST_MANIFEST = $(IMAGES_DIR)/gnu-test/os-real-gnu.manifest
GNU_TEST_MANIFEST_FILTER = awk '$$1 !~ /^\#/ && ($$3 == "/src" || index($$3, "/src/") == 1) { next } { print }'
MANIFEST_DEPS_gnu_test := $(shell $(GNU_TEST_MANIFEST_FILTER) $(GNU_ROOT_MANIFEST) | $(MANIFEST_DEPS) -)
GNU_ROOT_MANIFEST_DEPS = $(GNU_ROOT_MANIFEST) $(MANIFEST_DEPS_os_real_gnu)
GNU_TEST_MANIFEST_DEPS = $(GNU_TEST_MANIFEST) $(MANIFEST_DEPS_gnu_test)
$(GNU_TEST_MANIFEST): $(GNU_ROOT_MANIFEST) mk/gnu-image.mk
	@mkdir -p $(@D)
	@$(GNU_TEST_MANIFEST_FILTER) $(GNU_ROOT_MANIFEST) > $@.new
	@if cmp -s $@.new $@; then rm -f $@.new; else mv $@.new $@; fi

os-real-gnu-ext2: $(OS_REAL_GNU_EXT2)
os-real-gnu-agents-ext2: $(OS_REAL_GNU_AGENTS_EXT2)

ifeq ($(GNU_TOOLING),)
$(GNU_TEST_EXT2): $(GNU_IMAGE_DEPS) $(GNU_TEST_MANIFEST_DEPS) $(GNU_AGENTS_LIST) $(GNU_AGENTS_STAMPS) \
    $(GNU_SANITIZERS_STAMP) $(GNU_HOST_PRODUCTS_STAMP) $(GNU_FSTOOLS_STAMP) \
    $(if $(filter x86_64,$(ARCH)),$(GNU_HOST_CHECKS_STAMP) $(GNU_VK_GL_CTS_STAMP)) | $(GNU_IMAGE_ORDER)
	RACH2_ARCH=$(ARCH) GNU_AGENTS="$(GNU_AGENTS_ON)" GNU_TEST_PRODUCTS=1 GNU_I386=$(GNU_I386_ON) \
	    GNU_VK_GL_CTS=$(if $(filter x86_64,$(ARCH)),1) \
	    sh tools/image/mkext2.sh --overlay vendor/gnu/install_root.sh \
	    $(GNU_TEST_MANIFEST) $@ 24576

GNU_IGT_ON = $(if $(filter x86_64,$(ARCH)),1)
$(OS_REAL_GNU_EXT2): $(GNU_IMAGE_DEPS) $(GNU_ROOT_MANIFEST_DEPS) $(GNU_AGENTS_LIST) $(GNU_AGENTS_STAMPS) $(GNU_LOGINS_STAMP) \
                     $(if $(GNU_IGT_ON),$(GNU_IGT_STAMP) $(GNU_VK_GL_CTS_STAMP)) tools/image/stage_logins.sh | $(GNU_IMAGE_ORDER)
	RACH2_ARCH=$(ARCH) GNU_AGENTS="$(GNU_AGENTS_ON)" GNU_IGT=$(GNU_IGT_ON) GNU_VK_GL_CTS=$(GNU_IGT_ON) \
	    GNU_I386=$(GNU_I386_ON) sh tools/image/mkext2.sh \
	    --overlay vendor/gnu/install_root.sh krffs/manifests/os-real-gnu.manifest $@ 16384
	$(GNU_STAGE_LOGINS)

$(OS_REAL_GNU_AGENTS_EXT2): $(GNU_IMAGE_DEPS) $(GNU_ROOT_MANIFEST_DEPS) $(GNU_AGENTS_ALL_LIST) $(GNU_AGENTS_ALL_STAMPS) | $(GNU_IMAGE_ORDER)
	RACH2_ARCH=$(ARCH) GNU_AGENTS="$(GNU_AGENTS_ALL)" GNU_I386=$(GNU_I386_ON) sh tools/image/mkext2.sh \
	    --overlay vendor/gnu/install_root.sh krffs/manifests/os-real-gnu.manifest $@ 16384
else
GNU_TOOLING_DEPS = $(filter-out $(GNU_IMAGE_STAMPS),$(GNU_IMAGE_DEPS)) \
                   $(sort $(GNU_TOOLCHAIN_SOURCES) $(GNU_PORTS_SOURCES) $(GNU_NATIVE_SOURCES) \
                          $(GNU_GDB_SOURCES) $(GNU_JS_SOURCES) $(GNU_DEBIAN_SOURCES) $(GNU_PROTON_SOURCES) \
                          $(GNU_WEBGL_SOURCES) $(GNU_VENUS_SOURCES) $(GNU_VA_SOURCES) $(GNU_GAMES_SOURCES) $(if $(GNU_I386_ON),$(GNU_I386_SOURCES)) \
                          $(if $(filter x86_64,$(ARCH)),$(GNU_KVM_SOURCES))) \
                   $(GNU_PORTS_LINK) vendor/gnu/tooling_image.sh vendor/gnu/tooling_job.sh \
                   tools/setup/elfwrap.sh
GNU_TOOLING_ORDER = $(GNU_IMAGE_ORDER) $(ETUDE) $(SHA256TOOL)
GNU_AGENTS_RECIPES = $(patsubst %,vendor/gnu/agents/packages/%.sh,$(filter $(GNU_AGENTS_ALL),$(GNU_AGENTS_ON)))
GNU_AGENTS_ALL_RECIPES = $(patsubst %,vendor/gnu/agents/packages/%.sh,$(GNU_AGENTS_ALL))

$(GNU_TEST_EXT2): $(GNU_TOOLING_DEPS) $(GNU_TEST_MANIFEST_DEPS) $(GNU_AGENTS_LIST) $(GNU_AGENTS_RECIPES) \
    $(GNU_SANITIZER_SOURCES) $(GNU_HOST_PRODUCTS_SOURCES) $(GNU_FSTOOLS_SOURCES) \
    $(if $(filter x86_64,$(ARCH)),$(GNU_HOST_CHECKS_SOURCES) \
        build/tests/host-suite/tools/exfat_external/exfat_external.img) \
    $(if $(GNU_AGENTS_RECIPES),$(GNU_AGENTS_SOURCES)) | $(GNU_TOOLING_ORDER)
	RACH2_ARCH=$(ARCH) GNU_AGENTS="$(GNU_AGENTS_ON)" GNU_LINK="$(GNU_LINK)" GNU_I386=$(GNU_I386_ON) \
	    GNU_TEST_PRODUCTS=1 GNU_MANIFEST=$(GNU_TEST_MANIFEST) MUSL_CRYPT_SRC=$(MUSL_CRYPT_SRC) \
	    sh vendor/gnu/tooling_image.sh $@ 24576

$(OS_REAL_GNU_EXT2): $(GNU_TOOLING_DEPS) $(GNU_ROOT_MANIFEST_DEPS) $(GNU_AGENTS_LIST) $(GNU_AGENTS_RECIPES) \
                     $(if $(GNU_AGENTS_RECIPES),$(GNU_AGENTS_SOURCES)) $(GNU_LOGINS_STAMP) \
                     tools/image/stage_logins.sh | $(GNU_TOOLING_ORDER)
	RACH2_ARCH=$(ARCH) GNU_AGENTS="$(GNU_AGENTS_ON)" GNU_LINK="$(GNU_LINK)" GNU_I386=$(GNU_I386_ON) \
	    MUSL_CRYPT_SRC=$(MUSL_CRYPT_SRC) sh vendor/gnu/tooling_image.sh $@ 16384
	$(GNU_STAGE_LOGINS)

$(OS_REAL_GNU_AGENTS_EXT2): $(GNU_TOOLING_DEPS) $(GNU_ROOT_MANIFEST_DEPS) $(GNU_AGENTS_ALL_LIST) $(GNU_AGENTS_ALL_RECIPES) \
                            $(GNU_AGENTS_SOURCES) | $(GNU_TOOLING_ORDER)
	RACH2_ARCH=$(ARCH) GNU_AGENTS="$(GNU_AGENTS_ALL)" GNU_LINK="$(GNU_LINK)" GNU_I386=$(GNU_I386_ON) \
	    MUSL_CRYPT_SRC=$(MUSL_CRYPT_SRC) sh vendor/gnu/tooling_image.sh $@ 16384
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
