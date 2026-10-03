# Rach2 root Makefile.
#
# The build produces purpose-built RAM-disk images, each its own file under
# build/images/ so they coexist: `test` (the automated end-to-end test
# binaries), `os-simple` (a small Unix built out of this project's own code: our
# login, shell and Unix tools), and `os-real` (the same jobs taken from the code
# a real system ships: busybox, dropbear, Doom, and the tcc compiler with
# pdpmake to drive it).
#
# Quick start. The build checks its own tools; `sh tools/setup/doctor.sh` is the
# wider machine report.
#   make            build everything: kernel, host tools, guest programs, the
#                   vendored userland, Doom, and the three RAM images. The
#                   first build compiles the five vendor projects and takes
#                   minutes; later runs rebuild only what changed.
#   make ARCH=x86_64  the same for x86-64, into build/x86_64/; ARCH is i686,
#                   into build/, when it is not given (mk/arch.mk).
#   make test       boot the test image and check every probe passes.
#   tests/run NAME  run one named suite (style, host, ssh, all, ...).
#   make isos       a bootable ISO per image, for real hardware (needs xorriso).
#   make clean      remove the in-house build products under build/ and the
#                   images
#   make distclean  + the vendored musl, busybox, dropbear, tcc and pdpmake
#                   builds (full from-scratch)

# Run a built image with rach2/run.sh <test|os-simple|os-real>; the build system
# does not run QEMU (that script models the disk/audio/network/-cpu matrix).

# The architecture (mk/arch.mk): ARCH, and the paths of its build tree
# (ARCH_BUILD) and of its ported musl (ARCH_MUSL). The host programs under
# build/tools and build/krffs serve both architectures and keep their paths.
include mk/arch.mk

# --- components (each builds via its own sub-make) ---
BASE_DIRS = krffs rach2 tests ish utils
FULL_DIRS = ports/doom

# --- vendored third-party: built by their own scripts (which wrap musl's
#     configure script, busybox's kbuild, dropbear's autoconf, tcc's
#     hand-written configure and
#     pdpmake's bare POSIX makefile), tracked with stamp files so a clean
#     clone "just works" without running the build_*.sh by hand, and each
#     rebuilds only when its real inputs (tarball, build script, config)
#     change. Each architecture has its own stamps, extracted trees and
#     staged files: an i686 port's work is under vendor/NAME/, an x86-64
#     port's under build/x86_64/vendor-work/NAME/ (ARCH_VENDOR), and the
#     build_*.sh scripts take the architecture from RACH2_ARCH (mk/arch.sh). ---
LIBC_STAMP     = $(ARCH_VENDOR)/musl/.stamp
BUSYBOX_STAMP  = $(ARCH_VENDOR)/busybox/.stamp
DROPBEAR_STAMP = $(ARCH_VENDOR)/dropbear/.stamp
TCC_STAMP      = $(ARCH_VENDOR)/tcc/.stamp
PDPMAKE_STAMP  = $(ARCH_VENDOR)/pdpmake/.stamp
E2FSPROGS_STAMP = $(ARCH_VENDOR)/e2fsprogs/.stamp
LIBC_OUTPUTS_LIST = $(ARCH_VENDOR)/musl/.outputs
TCC_OUTPUTS_LIST  = $(ARCH_VENDOR)/tcc/.outputs
TCC_VALIDATED     = $(ARCH_VENDOR)/tcc/.validated
PDPMAKE_VALIDATED = $(ARCH_VENDOR)/pdpmake/.validated
# What each port's build script and its stamp rule share: the wrapper every
# port but musl compiles through, and the architecture table both read.
VENDOR_COMMON = vendor/musl/muslgcc mk/arch.sh

# The staged capstone binaries the build_*.sh scripts produce, out of tree under
# build/vendor/, or build/x86_64/vendor/ (mirroring their vendor/<name>/
# sources). A routine `make clean`
# never reaches build/vendor, so these expensive artifacts survive it; only
# `make distclean` removes them. They are the REAL prerequisites of the capstone
# image (see the check targets below): a stamp only proves its build RAN.
VENDOR_STAGE = $(ARCH_BUILD)/vendor
BUSYBOX_STAGE = $(VENDOR_STAGE)/busybox
TCC_STAGE    = $(VENDOR_STAGE)/tcc
BUSYBOX_BIN  = $(BUSYBOX_STAGE)/busybox
BUSYBOX_SUID_BIN = $(BUSYBOX_STAGE)/busybox-suid
DROPBEAR_BIN = $(VENDOR_STAGE)/dropbear/dropbearmulti
TCC_BIN      = $(TCC_STAGE)/tcc
PDPMAKE_BIN  = $(VENDOR_STAGE)/pdpmake/make
E2FSCK_BIN   = $(VENDOR_STAGE)/e2fsprogs/e2fsck
RESIZE2FS_BIN = $(VENDOR_STAGE)/e2fsprogs/resize2fs
MKE2FS_BIN   = $(VENDOR_STAGE)/e2fsprogs/mke2fs
TUNE2FS_BIN  = $(VENDOR_STAGE)/e2fsprogs/tune2fs
DUMPE2FS_BIN = $(VENDOR_STAGE)/e2fsprogs/dumpe2fs

# The pinned vendored source tarballs: the exact names the build_*.sh scripts
# verify by sha256. Used as stamp prerequisites (not a $(wildcard), which would
# silently drop the prereq and look up-to-date if a tarball were deleted) and to
# derive the extracted source dirs that `distclean` removes.
LIBC_TARBALL     = vendor/musl/musl-1.2.5.tar.gz
BUSYBOX_TARBALL  = vendor/busybox/busybox-1.36.1.tar.bz2
DROPBEAR_TARBALL = vendor/dropbear/dropbear-2022.83.tar.bz2
TCC_TARBALL      = vendor/tcc/tcc-0.9.27.tar.bz2
PDPMAKE_TARBALL  = vendor/pdpmake/pdpmake-2.0.4.tgz
PDPMAKE_PATCHES  = vendor/pdpmake/patches/0001-platform-macros.patch
E2FSPROGS_TARBALL = vendor/e2fsprogs/e2fsprogs-1.47.4.tar.gz
E2FSPROGS_PATCHES = vendor/e2fsprogs/patches/series $(wildcard vendor/e2fsprogs/patches/*.patch)

# The two musl artifacts consumers depend on: the installed static libc the
# cross-compiled userland links against, at a fixed path under the install
# directory, and the extracted source of the SHA-512 crypt, whose path comes
# from the tarball name so the pinned version is written once. The
# host-compiled mkhash and its host test read that source, which is the same
# file in either architecture's tree, so krffs takes it from the tree of the
# architecture being built.
LIBC_A         = $(ARCH_MUSL)/lib/libc.a
MUSL_CRYPT_SRC = $(ARCH_VENDOR)/musl/$(notdir $(LIBC_TARBALL:.tar.gz=))/src/crypt/crypt_sha512.c

# The kernel and the images of this architecture.
KERNEL          = $(ARCH_BUILD)/rach2/kernel
IMAGES_DIR      = $(ARCH_BUILD)/images
TEST_IMAGE      = $(IMAGES_DIR)/test.krffs
OS_SIMPLE_IMAGE = $(IMAGES_DIR)/os-simple.krffs
OS_REAL_IMAGE   = $(IMAGES_DIR)/os-real.krffs
OS_REAL_EXT2    = $(IMAGES_DIR)/os-real.ext2
METAL_IMAGE     = $(IMAGES_DIR)/rach2-metal.krffs
OS_REAL_DISK    = $(IMAGES_DIR)/os-real.disk
OS_REAL_GRUB_DISK = $(IMAGES_DIR)/os-real-grub$(RECOVERY_KEY_TAG).disk
MKIMAGE         = krffs/mkimage.sh
BINFIND         = build/tools/inspect/binfind
ELFCHECK        = build/tools/inspect/elfcheck
SETIDCHECK      = build/tools/inspect/setidcheck
CFGCHECK        = build/tools/inspect/cfgcheck
PROSECHECK      = build/tools/verify/prosecheck
SIZECHECK       = build/tools/verify/sizecheck
GUESTCHECK      = build/tools/verify/guestcheck
SHA256TOOL      = build/tools/verify/sha256
MKDISK          = krffs/mkdisk.sh

.PHONY: all clean distclean images musl-programs test preflight check-sources prepare-all \
        test-image os-simple-image os-real-image os-real-disk \
        rach2-metal rach2-metal-image \
        rach2-sealed-disk rach2-recovery rach2-recovery-image \
        isos test-iso os-simple-iso os-real-iso rach2-metal-iso \
        check-busybox check-setid check-dropbearmulti check-libc check-tcc \
        check-pdpmake check-e2fsck \
        run-style run-host run-default \
        musl busybox dropbear tcc pdpmake e2fsprogs $(BASE_DIRS) $(FULL_DIRS)

# Default: everything reusable. The three RAM images pull in the kernel, the
# host tools, the guest programs, the vendored userland, and Doom through their
# own prerequisites. The first build compiles five vendor projects and takes
# minutes; a later `make` rebuilds only what changed. Each image is its own file
# under build/images/, so they coexist instead of overwriting one shared image.
# `make` builds the product: the kernel, the userland, the three bootable
# images, and the validators that judge them, along with the host tools, which
# include the programs the suites drive machines with. What it does not build
# is the per-suite material: the fixtures, the variant kernels and the images
# only one suite boots live in tests/system/suites.mk, and each suite target
# builds what it invokes.
all: images host-tools

# The cached tool check (tools/setup/preflight.sh): a missing build tool fails
# here, named, and a changed one makes the check run again. It is an
# order-only prerequisite of the build entry points, so under `make -j` a host
# tool can compile beside it; what it guards is the compilation that needs the
# cross toolchain. clean and distclean never run it.
preflight:
	@RACH2_ARCH=$(ARCH) ./tools/setup/preflight.sh

all images $(BASE_DIRS) $(FULL_DIRS): | preflight
$(LIBC_STAMP) $(BUSYBOX_STAMP) $(DROPBEAR_STAMP) $(TCC_STAMP) $(PDPMAKE_STAMP) \
    $(E2FSPROGS_STAMP): | preflight

# Friendly aliases for the third-party stamps.
musl: $(LIBC_STAMP)
busybox: $(BUSYBOX_STAMP)
dropbear: $(DROPBEAR_STAMP)
tcc: $(TCC_STAMP)
pdpmake: $(PDPMAKE_STAMP)
e2fsprogs: $(E2FSPROGS_STAMP)

include mk/dryrun.mk

# --- third-party stamp rules. Each removes BOTH the stamp and the expected
#     artifact up front, runs its build script, requires a non-empty
#     artifact, then touches the stamp. Removing the artifact first means a
#     script that exits 0 without restaging cannot pass the assertion on a
#     STALE artifact; removing the stamp first means any failure (build or
#     assertion) leaves no stamp, so the next make retries instead of
#     trusting a missing or old artifact. .DELETE_ON_ERROR is the backstop on
#     the final touch. ---
.DELETE_ON_ERROR:

# The patch manifest and the pinned patches are prerequisites, not decoration:
# build_libc.sh applies the patches patches/series names after the extract, so a
# tree whose stamp predates a series edit would keep linking every program
# against the wrong libc, and an edit to one patch would never be built at all.
# The series file notices an added or removed patch; the wildcard notices an
# edit to one that exists.
LIBC_PATCHES = vendor/musl/patches/series $(wildcard vendor/musl/patches/*.patch)

$(LIBC_STAMP): vendor/musl/build_libc.sh mk/arch.sh $(LIBC_TARBALL) $(LIBC_PATCHES)
	@rm -f $@ $(LIBC_A)
	+@if [ -n "$(DRY_RUN)" ]; then echo "$@: dry run"; else t0=$$(date +%s); RACH2_ARCH=$(ARCH) ./vendor/musl/build_libc.sh && echo "libc: result: built in $$(($$(date +%s)-t0))s"; fi
	@test -s $(LIBC_A) || { echo "libc: build produced no libc.a" >&2; exit 1; }
	@{ find $(ARCH_MUSL) -type f | sort; echo $(MUSL_CRYPT_SRC); } > $(LIBC_OUTPUTS_LIST)
	@touch $@

# The patches are a prerequisite for the same reason musl's are:
# `build_busybox.sh` applies everything under vendor/busybox/patches/ after it
# extracts, so a stamp that did not depend on them would keep shipping a binary
# built from the patch before the edit. The directory is a prerequisite beside
# the wildcard: the wildcard notices an edit to a patch that exists, and the
# directory's own timestamp notices one being added or taken away, including the
# last one.
BUSYBOX_PATCHES = vendor/busybox/patches $(wildcard vendor/busybox/patches/*.patch)

$(BUSYBOX_STAMP): vendor/busybox/build_busybox.sh vendor/busybox/rach2.config \
                  vendor/busybox/rach2-suid.config $(VENDOR_COMMON) \
                  $(BUSYBOX_TARBALL) $(BUSYBOX_PATCHES) $(LIBC_STAMP)
	@rm -f $@ $(BUSYBOX_BIN) $(BUSYBOX_SUID_BIN) \
	    $(BUSYBOX_STAGE)/applet_tables-main.h \
	    $(BUSYBOX_STAGE)/applet_tables-suid.h \
	    $(BUSYBOX_STAGE)/config-main \
	    $(BUSYBOX_STAGE)/config-suid
	+@if [ -n "$(DRY_RUN)" ]; then echo "$@: dry run"; else t0=$$(date +%s); RACH2_ARCH=$(ARCH) ./vendor/busybox/build_busybox.sh && echo "busybox: result: built in $$(($$(date +%s)-t0))s"; fi
	@test -s $(BUSYBOX_BIN) || { echo "busybox: build produced no $(BUSYBOX_BIN)" >&2; exit 1; }
	@touch $@

$(TCC_STAMP): vendor/tcc/build_tcc.sh vendor/tcc/rach2-output-policy.patch \
              vendor/tcc/rach2-x86_64-static-link.patch \
              $(VENDOR_COMMON) $(TCC_TARBALL) $(LIBC_STAMP)
	@rm -f $@ $(TCC_BIN) $(TCC_STAGE)/libtcc1.a \
	    $(TCC_STAGE)/include/stdarg.h
	+@if [ -n "$(DRY_RUN)" ]; then echo "$@: dry run"; else t0=$$(date +%s); RACH2_ARCH=$(ARCH) ./vendor/tcc/build_tcc.sh && echo "tcc: result: built in $$(($$(date +%s)-t0))s"; fi
	@test -s $(TCC_BIN) || { echo "tcc: build produced no $(TCC_BIN)" >&2; exit 1; }
	@find $(TCC_STAGE) -type f | sort > $(TCC_OUTPUTS_LIST)
	@touch $@

# pdpmake, the make that runs ON the machine. Its patches are prerequisites for
# the reason musl's are: they are applied at extraction, so a stamp that
# predates one would keep a make whose built-in rules name a compiler this image
# does not have and whose default link this system's compiler refuses.
$(PDPMAKE_STAMP): vendor/pdpmake/build_pdpmake.sh $(PDPMAKE_PATCHES) \
                  $(VENDOR_COMMON) $(PDPMAKE_TARBALL) $(LIBC_STAMP)
	@rm -f $@ $(PDPMAKE_BIN)
	+@if [ -n "$(DRY_RUN)" ]; then echo "$@: dry run"; else t0=$$(date +%s); RACH2_ARCH=$(ARCH) ./vendor/pdpmake/build_pdpmake.sh && echo "pdpmake: result: built in $$(($$(date +%s)-t0))s"; fi
	@test -s $(PDPMAKE_BIN) || { echo "pdpmake: build produced no $(PDPMAKE_BIN)" >&2; exit 1; }
	@touch $@

# e2fsprogs, of which the images stage e2fsck, resize2fs, mke2fs, tune2fs
# and dumpe2fs. The patch manifest is a prerequisite as Dropbear's is below,
# and mke2fs.conf is one because mke2fs compiles it in as its defaults.
$(E2FSPROGS_STAMP): vendor/e2fsprogs/build_e2fsprogs.sh $(VENDOR_COMMON) \
                    vendor/e2fsprogs/mke2fs.conf \
                    $(E2FSPROGS_PATCHES) $(E2FSPROGS_TARBALL) $(LIBC_STAMP)
	@rm -f $@ $(E2FSCK_BIN) $(RESIZE2FS_BIN) $(MKE2FS_BIN) $(TUNE2FS_BIN) $(DUMPE2FS_BIN)
	+@if [ -n "$(DRY_RUN)" ]; then echo "$@: dry run"; else t0=$$(date +%s); RACH2_ARCH=$(ARCH) ./vendor/e2fsprogs/build_e2fsprogs.sh && echo "e2fsprogs: result: built in $$(($$(date +%s)-t0))s"; fi
	@test -s $(E2FSCK_BIN) || { echo "e2fsprogs: build produced no $(E2FSCK_BIN)" >&2; exit 1; }
	@test -s $(RESIZE2FS_BIN) || { echo "e2fsprogs: build produced no $(RESIZE2FS_BIN)" >&2; exit 1; }
	@test -s $(MKE2FS_BIN) || { echo "e2fsprogs: build produced no $(MKE2FS_BIN)" >&2; exit 1; }
	@test -s $(TUNE2FS_BIN) || { echo "e2fsprogs: build produced no $(TUNE2FS_BIN)" >&2; exit 1; }
	@test -s $(DUMPE2FS_BIN) || { echo "e2fsprogs: build produced no $(DUMPE2FS_BIN)" >&2; exit 1; }
	@touch $@

# The patch manifest is a prerequisite for the reason musl's is: the script
# applies what patches/series names, so a series edit or a patch edit must
# invalidate the stamp.
DROPBEAR_PATCHES = vendor/dropbear/patches/series $(wildcard vendor/dropbear/patches/*.patch)

$(DROPBEAR_STAMP): vendor/dropbear/build_dropbear.sh vendor/dropbear/localoptions.h $(VENDOR_COMMON) \
                   $(DROPBEAR_PATCHES) $(DROPBEAR_TARBALL) $(LIBC_STAMP)
	@rm -f $@ $(DROPBEAR_BIN)
	+@if [ -n "$(DRY_RUN)" ]; then echo "$@: dry run"; else t0=$$(date +%s); RACH2_ARCH=$(ARCH) ./vendor/dropbear/build_dropbear.sh && echo "dropbear: result: built in $$(($$(date +%s)-t0))s"; fi
	@test -s $(DROPBEAR_BIN) || { echo "dropbear: build produced no $(DROPBEAR_BIN)" >&2; exit 1; }
	@touch $@

# --- vendor output healing. The stamps prove the scripts ran; the files the
#     scripts staged are what the graph consumes. A deleted output retires its
#     vendor's stamp HERE, at parse time, so the ordinary stamp rule rebuilds
#     the vendor and every transitive dependent in the same run; a recipe-time
#     sub-make could not, because make's cached timestamps would miss the
#     stamp's change. The dependency-only rules below make each output a known
#     target ordered behind its stamp. A binary that cannot legitimately be
#     empty is size-tested below as well, so a truncated one rebuilds its
#     vendor rather than failing a later check. musl and TinyCC declare their
#     whole staged trees in .outputs lists their stamp recipes write. ---
MUSL_OUTPUTS = $(shell cat $(LIBC_OUTPUTS_LIST) 2>/dev/null)
TCC_OUTPUTS = $(shell cat $(TCC_OUTPUTS_LIST) 2>/dev/null)
BUSYBOX_OUTPUTS = $(BUSYBOX_BIN) $(BUSYBOX_SUID_BIN) \
                  $(BUSYBOX_STAGE)/applet_tables-main.h \
                  $(BUSYBOX_STAGE)/applet_tables-suid.h \
                  $(BUSYBOX_STAGE)/config-main \
                  $(BUSYBOX_STAGE)/config-suid

_ := $(shell [ ! -f $(LIBC_STAMP) ] || [ ! -f $(LIBC_OUTPUTS_LIST) ] || \
        while IFS= read -r f; do [ -e "$$f" ] || { rm -f $(LIBC_STAMP); break; }; done \
            < $(LIBC_OUTPUTS_LIST))
_ := $(shell [ ! -f $(TCC_STAMP) ] || [ ! -f $(TCC_OUTPUTS_LIST) ] || \
        while IFS= read -r f; do [ -e "$$f" ] || { rm -f $(TCC_STAMP); break; }; done \
            < $(TCC_OUTPUTS_LIST))
_ := $(shell [ ! -f $(BUSYBOX_STAMP) ] || \
        for f in $(BUSYBOX_OUTPUTS); do [ -e "$$f" ] || { rm -f $(BUSYBOX_STAMP); break; }; done)
_ := $(shell [ ! -f $(DROPBEAR_STAMP) ] || [ -s $(DROPBEAR_BIN) ] || rm -f $(DROPBEAR_STAMP))
_ := $(shell [ ! -f $(PDPMAKE_STAMP) ] || [ -s $(PDPMAKE_BIN) ] || rm -f $(PDPMAKE_STAMP))
_ := $(shell [ ! -f $(E2FSPROGS_STAMP) ] || { [ -s $(E2FSCK_BIN) ] && [ -s $(RESIZE2FS_BIN) ] && [ -s $(MKE2FS_BIN) ] && [ -s $(TUNE2FS_BIN) ] && [ -s $(DUMPE2FS_BIN) ]; } || rm -f $(E2FSPROGS_STAMP))

# The tree lists above test existence only, because musl installs several
# legitimately empty headers. The binaries that can never be empty also get a
# size test, so a truncated one rebuilds its vendor instead of failing a check
# with advice.
_ := $(shell [ ! -f $(LIBC_STAMP) ] || [ -s $(LIBC_A) ] || rm -f $(LIBC_STAMP))
_ := $(shell [ ! -f $(TCC_STAMP) ] || [ -s $(TCC_BIN) ] || rm -f $(TCC_STAMP))
_ := $(shell [ ! -f $(BUSYBOX_STAMP) ] || { [ -s $(BUSYBOX_BIN) ] && [ -s $(BUSYBOX_SUID_BIN) ]; } || rm -f $(BUSYBOX_STAMP))

# Before the first build there is no .outputs list, yet the manifests already
# name staged files as prerequisites of the images, and a parallel make from
# a clean tree reached an image before the stamp and stopped with no rule
# for libc.a. The staged files the manifests name are read from the
# manifests here, so they are known targets from the first make on. These
# rules have no recipe on purpose: a rule with one, even an empty one, marks
# the file remade whenever the stamp is newer, and the stamp is always newer
# than what it staged, so every image would be restaged by every make.
# The files each manifest stages, read once here and used by every rule that
# builds an image from that manifest.
#
# A manifest names its host files by their i686 paths, build/... and
# vendor/musl/install/..., and so does the set-id allowlist that judges it, so
# one manifest describes the image of either architecture. For x86-64 the
# stager, the set-id guard and manifest_deps.sh read each such path from the
# x86-64 tree instead, by the prefixes RACH2_PATH_MAP lists in order, the
# first that matches taking the path. The manual's pages and the suites'
# fixtures under build/tests/system are host files and stay where they are.
# The map is exported to the recipes and given by name to the parse-time
# reads below, since whether $(shell) sees an exported variable has differed
# between versions of GNU Make.
ifneq ($(ARCH_SUFFIX),)
RACH2_PATH_MAP = build/man/=build/man/ build/tests/system/=build/tests/system/ \
                 build/=$(ARCH_BUILD)/ vendor/musl/install/=$(ARCH_MUSL)/
export RACH2_PATH_MAP
endif
MANIFEST_DEPS = RACH2_PATH_MAP='$(RACH2_PATH_MAP)' sh krffs/manifest_deps.sh
MANIFEST_DEPS_test := $(shell $(MANIFEST_DEPS) krffs/manifests/test.manifest)
MANIFEST_DEPS_os_simple := $(shell $(MANIFEST_DEPS) krffs/manifests/os-simple.manifest)
MANIFEST_DEPS_os_real := $(shell $(MANIFEST_DEPS) krffs/manifests/os-real.manifest)
MANIFEST_DEPS_os_real_gnu := $(shell $(MANIFEST_DEPS) krffs/manifests/os-real-gnu.manifest)
MANIFEST_DEPS_rach2_metal := $(shell $(MANIFEST_DEPS) krffs/manifests/rach2-metal.manifest)
MANIFEST_DEPS_rach2_recovery := $(shell $(MANIFEST_DEPS) krffs/manifests/rach2-recovery.manifest)
MANIFEST_DEPS_rach2_metal_disk := $(shell $(MANIFEST_DEPS) krffs/manifests/rach2-metal-disk.manifest)
MANIFEST_DEPS_grub_test := $(shell $(MANIFEST_DEPS) krffs/manifests/grub-test.manifest)
MANIFEST_INPUTS = $(sort $(MANIFEST_DEPS_test) $(MANIFEST_DEPS_os_simple) \
    $(MANIFEST_DEPS_os_real) $(MANIFEST_DEPS_os_real_gnu) $(MANIFEST_DEPS_rach2_metal) \
    $(MANIFEST_DEPS_rach2_recovery) $(MANIFEST_DEPS_rach2_metal_disk) $(MANIFEST_DEPS_grub_test))
STAGED_IN_MANIFESTS = $(filter $(ARCH_MUSL)/% $(TCC_STAGE)/%,$(MANIFEST_INPUTS))
MUSL_KNOWN = $(sort $(MUSL_OUTPUTS) $(filter $(ARCH_MUSL)/%,$(STAGED_IN_MANIFESTS)))
TCC_KNOWN = $(sort $(TCC_OUTPUTS) $(filter $(TCC_STAGE)/%,$(STAGED_IN_MANIFESTS)))
ifneq ($(MUSL_KNOWN),)
$(MUSL_KNOWN): $(LIBC_STAMP)
endif
ifneq ($(TCC_KNOWN),)
$(TCC_KNOWN): $(TCC_STAMP)
endif
$(BUSYBOX_OUTPUTS): $(BUSYBOX_STAMP)
$(DROPBEAR_BIN): $(DROPBEAR_STAMP)
$(PDPMAKE_BIN): $(PDPMAKE_STAMP)
$(E2FSCK_BIN) $(RESIZE2FS_BIN) $(MKE2FS_BIN) $(TUNE2FS_BIN) $(DUMPE2FS_BIN): $(E2FSPROGS_STAMP)

# --- component files the root graph names. Each gets an explicit delegation
#     rule to its sub-make: without one, a deleted kernel beside its surviving
#     kernel.o would match make's builtin link rule and be relinked by the
#     host cc. Each rule asks its sub-make on every invocation, through the
#     phony target below, and the sub-make decides whether anything is
#     rebuilt. ---
# The three kernels are built by sub-makes that do their own dependency
# tracking. These rules have no prerequisites of their own, so they ask the
# sub-make on every invocation and let it decide: it relinks only when a source
# changed, which moves the file's timestamp only then, so an image naming the
# file rebuilds exactly when the kernel really changed and not otherwise.
#
# Without this the rules fired only when the file was missing. On 2026-09-14 a
# disk image shipped a kernel older than the tree, and what caught it was a
# person reading a boot log for a message that had been deleted.
.PHONY: ask-the-sub-make
ask-the-sub-make:

$(KERNEL): ask-the-sub-make
	@$(MAKE) --no-print-directory -C rach2
$(ARCH_BUILD)/ish/ish: ask-the-sub-make
	@$(MAKE) --no-print-directory -C ish
# One of the three asks the sub-make and the other two wait for it. A rule with
# several targets is a rule per target to make, so the three missing files of a
# clean build were three sub-makes of one directory at once.
build/krffs/mkfs.krffs: ask-the-sub-make | check-libc
	@$(MAKE) --no-print-directory -C krffs $(if $(ARCH_SUFFIX),MUSL_CRYPT=../$(MUSL_CRYPT_SRC))
build/krffs/stageimage.krffs build/krffs/fsck.krffs: build/krffs/mkfs.krffs
	@:

# A staged file waits for its component, then make reads its time again.
# Each component still runs once, even when several images stage its files.
# The empty recipes run under -n too, without claiming a file has changed.
$(filter-out $(ARCH_BUILD)/tests/musl/%,$(filter $(ARCH_BUILD)/tests/%,$(MANIFEST_INPUTS))): tests
	+@:
$(filter $(ARCH_BUILD)/tests/musl/%,$(MANIFEST_INPUTS)): musl-programs
	+@:
$(filter $(ARCH_BUILD)/utils/%,$(MANIFEST_INPUTS)): utils
	+@:
$(filter $(ARCH_BUILD)/ports/doom/%,$(MANIFEST_INPUTS)): ports/doom
	+@:

# The binary byte searcher (tools/inspect/binfind.c), replacing strings(1) piped
# to grep in the TinyCC policy check.
$(BINFIND): tools/inspect/binfind.c
	@mkdir -p $(dir $@)
	$(HOST_LINK)

# The ELF validator (tools/inspect/elfcheck.c): user executables against the
# loader and the static-only policy, kernels against the multiboot loaders.
$(ELFCHECK): tools/inspect/elfcheck.c
	@mkdir -p $(dir $@)
	$(HOST_LINK)

# The set-id guard (tools/inspect/setidcheck.c), reading its declarations from
# tools/inspect/setid.allow.
$(SETIDCHECK): tools/inspect/setidcheck.c
	@mkdir -p $(dir $@)
	$(HOST_LINK)

# The configuration checkers (tools/inspect/cfgcheck.c): the GRUB core's
# embedded configuration and the sealed disk's fixed address.
$(CFGCHECK): tools/inspect/cfgcheck.c
	@mkdir -p $(dir $@)
	$(HOST_LINK)

# The prose checks (tools/verify/prosecheck.c): comment width, the banned
# vocabulary, the phrase counts the report tabulates, and the dev-log index.
$(PROSECHECK): tools/verify/prosecheck.c
	@mkdir -p $(dir $@)
	$(HOST_LINK)

# The size limits (tools/verify/sizecheck.c): function lines, parameters,
# nesting, and the complexity pmccabe measures, judged against the ceilings
# in tools/verify/sizes.reasons.
$(SIZECHECK): tools/verify/sizecheck.c
	@mkdir -p $(dir $@)
	$(HOST_LINK)

# The byte scanner (tools/verify/asciiscan.c) the text checks run, the host
# check (tools/setup/hostcheck.c) `tests/run` compiles for itself before a
# build, and the TCP measurer (tools/dev/machine_net.c) of the machine
# runs. They are built here so a `make` compiles them under the host flags
# and the compilation database lists them.
ASCIISCAN  = build/tools/verify/asciiscan
HOSTCHECK  = build/tools/setup/hostcheck
MACHINENET = build/tools/dev/machine_net
$(ASCIISCAN): tools/verify/asciiscan.c
	@mkdir -p $(dir $@)
	$(HOST_LINK)
$(HOSTCHECK): tools/setup/hostcheck.c
	@mkdir -p $(dir $@)
	$(HOST_LINK)
$(MACHINENET): tools/dev/machine_net.c
	@mkdir -p $(dir $@)
	$(HOST_LINK)

# The guest-build guard (tools/verify/guestcheck.c): the three guest makefiles
# against the host build they mirror.
$(GUESTCHECK): tools/verify/guestcheck.c
	@mkdir -p $(dir $@)
	$(HOST_LINK)

# The digest tool (tools/verify/sha256.c), replacing sha256sum and shasum. The
# scripts that need it before make has run compile it themselves.
$(SHA256TOOL): tools/verify/sha256.c
	@mkdir -p $(dir $@)
	$(HOST_LINK)

# --- validation stamps. A validator is a prerequisite of a stamp that sits
#     between a published artifact and its consumers, so editing the checker
#     revalidates the artifact without recompiling it. Validation of a fresh
#     candidate happens inside the producing recipe (the kernel link, mkimage)
#     and preserves the previous artifact on failure; a failed REvalidation
#     here removes the published artifact and its stamp and fails, so the
#     next run rebuilds it through the ordinary rules. fsck would clear the
#     image's dirty flag in place, so the image stamp runs only the
#     non-mutating set-id inventory. ---
$(ARCH_BUILD)/rach2/.kernel-checked: $(KERNEL) $(ELFCHECK)
	@$(ELFCHECK) --kernel $(KERNEL) || { rm -f $(KERNEL) $@; exit 1; }
	@touch $@

$(TCC_VALIDATED): $(TCC_STAMP) tools/inspect/check_link.sh
	@CHECK_LINK_NM=$(CROSS_PREFIX)nm sh tools/inspect/check_link.sh $(LIBC_A) $(TCC_STAGE)/libtcc1.a \
	    $(ARCH_MUSL)/lib/crt1.o $(ARCH_MUSL)/lib/crti.o \
	    $(ARCH_MUSL)/lib/crtn.o || { rm -rf $(TCC_STAGE) $@; exit 1; }
	@touch $@

$(PDPMAKE_VALIDATED): $(PDPMAKE_STAMP) $(ELFCHECK)
	@$(ELFCHECK) --arch $(ARCH) $(PDPMAKE_BIN) || { rm -f $(PDPMAKE_BIN) $@; exit 1; }
	@touch $@

$(IMAGES_DIR)/%.checked: $(IMAGES_DIR)/%.krffs $(SETIDCHECK) tools/inspect/setid.allow
	@$(SETIDCHECK) --image $< --manifest krffs/manifests/$*.manifest \
	    || { rm -f $< $@; exit 1; }
	@touch $@

all images: $(ARCH_BUILD)/rach2/.kernel-checked $(TCC_VALIDATED) $(PDPMAKE_VALIDATED)
test-image: $(IMAGES_DIR)/test.checked
os-simple-image: $(IMAGES_DIR)/os-simple.checked
os-real-image: $(IMAGES_DIR)/os-real.checked

# --- component sub-makes + the build-order DAG ---
#
# `rach2`, `ish` and `krffs` are not here: each has a file rule above that
# runs its sub-make, and a component name that ran one as well gave two
# routes to the same sub-make with no edge between them. Under `make -j`
# both ran at once, compiled every object twice into the same files, and the
# link read an object the other job was writing: a clean parallel build on
# 2026-09-14 ended in `undefined reference to krandom_seed`. Each of the
# three now asks for its file, and every one of those file rules asks its
# sub-make on each invocation, which is what keeps an edited source built.
$(filter-out rach2 ish krffs,$(BASE_DIRS)) $(FULL_DIRS):
	@echo "$(notdir $@): building"
	@$(MAKE) --no-print-directory -C $@

rach2: $(KERNEL)
ish: $(ARCH_BUILD)/ish/ish
krffs: build/krffs/mkfs.krffs

# krffs waits on the vendored musl because mkhash compiles its SHA-512 crypt
# from that source, which exists only once build_libc.sh has extracted the
# pinned tarball; without this a from-scratch tree (a fresh clone, or make
# distclean) builds krffs first and cannot find the file. Only real edges: krffs
# compiles the extracted musl crypt source, and tests and utils link the
# installed musl. The kernel, ISH, Doom, and the KRFFS tools share no build
# inputs with one another, so `make -j` overlaps them.
krffs: check-libc

# The components that compile or stage musl's installed files list them as
# prerequisites, so a deleted header, library, or the extracted crypt source
# heals through the rules above before a sub-make would fail on it.
ifneq ($(MUSL_OUTPUTS),)
krffs tests utils: $(MUSL_OUTPUTS)
endif
tests: $(LIBC_STAMP)
utils: $(LIBC_STAMP)

# --- the RAM-disk images. Each is a real file target staged from its manifest
#     with mkimage.sh (dd + mkfs.krffs + stageimage.krffs), out of tree into
#     build/images/. The stager records every host input it read into an
#     image-side .d file, included below, so an untouched tree restages
#     nothing and a changed, added, or removed input restages exactly the
#     images that carry it. Staged component files wait for their builders
#     before make reads their times again. Order-only components also build
#     the files absent when make first reads a clean tree's manifests. ---

images: test-image os-simple-image os-real-image os-real-ext2

# The manual pages: mdoc under man/, one file per page in its section's
# directory, rendered by man/render.sh to plain text under build/man/catN,
# which the manifests stage under /usr/share/man, and to HTML under
# build/man/html for reading off the machine. An image that carries pages
# names $(MAN_CAT) among its prerequisites, so a changed page restages it.
MAN_SOURCES = $(wildcard man/man[0-9]/*.[0-9])
MAN_CAT = $(patsubst man/man%,build/man/cat%,$(MAN_SOURCES))
build/man/cat%: man/man% man/render.sh | preflight
	sh man/render.sh $< build/man
# The aliases are a table the images stage as it is, man/aliases, which
# the man script reads; the manifests name it, so the image rules depend
# on it through their manifest deps.
MAN_ALL = $(MAN_CAT)
.PHONY: man
man: $(MAN_ALL)
MANCHECK = build/tools/verify/mancheck
$(MANCHECK): tools/verify/mancheck.c
	@mkdir -p build/tools/verify
	$(HOST_LINK)
# `make mancheck`: every program, applet and call has a page; see the tool.
.PHONY: mancheck
mancheck: $(MANCHECK) $(BUSYBOX_STAMP)
	$(MANCHECK) man rach2/sys/arch/i686/sysnr.h rach2/sys/arch/i686/systab.c:rach2/sys/syscall_net.c $(BUSYBOX_STAGE)/applet_tables-main.h \
	    utils/programs.mk krffs/manifests/rach2-recovery.manifest krffs/manifests/os-simple.manifest \
	    krffs/manifests/os-real.manifest krffs/manifests/rach2-metal-disk.manifest

test-image: $(TEST_IMAGE)
os-simple-image: $(OS_SIMPLE_IMAGE)
os-real-image: $(OS_REAL_IMAGE)
os-real-ext2: $(OS_REAL_EXT2)

# The demo credentials, as file targets. Both generators are idempotent and do
# nothing when their output is there, which was enough while one thing ran at a
# time: with `make -j` and a pool of suites, two of them mint a key at once and
# race on the temporary file, which is `mv: authorized_keys.tmp: No such file
# or directory`. Make builds each once, and everything that needs them says so.
krffs/authorized_keys:
	sh krffs/gen_demo_key.sh

# The same generator writes the test account's key, which the images name
# too. The rule is ordered behind the root key so the two never mint at once,
# and a run of the generator leaves a key that is there alone, so this mints
# only a test key that is missing on its own.
krffs/test_authorized_keys: | krffs/authorized_keys
	sh krffs/gen_demo_key.sh

# The password hashes are minted by the krffs hasher, so a tree with no build
# directory yet builds the krffs tools before it mints; order-only, since a
# rebuilt hasher must not remint hashes the images already carry. The script
# is a prerequisite, so a change to the lines it writes reaches a tree that
# has a shadow: it keeps the hashes, brings the other fields up to date and
# touches the file.
krffs/shadow: krffs/gen_demo_password.sh | build/krffs/mkfs.krffs
	sh krffs/gen_demo_password.sh

# `make recovery-key`: put this tree's own key into the recovery system, so a
# machine built here can be updated over the network. An image built without
# that key starts no SSH server at all and the only way in is the keyboard,
# which is what its absence means. `tests/run` mints it before it builds
# anything, since the suites that update a machine do it through recovery over
# SSH; a plain `make` does not. RECOVERY_EXTRA_KEYS names an operator's public
# key file; when set, recovery-key replaces the recovery list with this tree's
# keys and that file. Without it, an existing recovery list is preserved.
.PHONY: recovery-key
recovery-key: krffs/authorized_keys
	sh krffs/gen_recovery_key.sh krffs/authorized_keys krffs/recovery_authorized_keys "$(RECOVERY_EXTRA_KEYS)"
	@echo "recovery-key: ready: krffs/recovery_authorized_keys"

# An order-only prerequisite, so the demo key being newer never overwrites a
# key somebody put here by hand: this rule mints one where there is none and
# leaves an existing one alone.
krffs/recovery_authorized_keys: | krffs/authorized_keys
	cp krffs/authorized_keys $@

# `make no-recovery-key`: the other direction, for an image somebody else runs.
# Every artifact that can embed a key goes with it, under both names. Make
# reads the name of a keyed artifact when it starts, so removing the key does
# not make one stale; and an image built before this name existed, or built in
# the same command that minted the key, carries a key under the plain name.
# One left behind would be written to a stick by a later command that named it.
.PHONY: no-recovery-key
no-recovery-key:
	rm -f krffs/recovery_authorized_keys
	rm -f $(IMAGES_DIR)/rach2-recovery-with-key.krffs \
	      $(IMAGES_DIR)/rach2-recovery-with-key.iso \
	      $(IMAGES_DIR)/rach2-install-stick-with-key.img \
	      $(IMAGES_DIR)/rach2-sealed-with-key.disk \
	      $(IMAGES_DIR)/os-real-grub-with-key.disk \
	      $(IMAGES_DIR)/rach2-recovery.krffs $(IMAGES_DIR)/rach2-recovery.iso \
	      $(IMAGES_DIR)/rach2-install-stick.img \
	      $(IMAGES_DIR)/install-test-stick.img \
	      $(IMAGES_DIR)/rach2-sealed.disk $(IMAGES_DIR)/os-real-grub.disk
	@echo "no-recovery-key: the recovery system will answer nobody"

# `make release`: the artifacts a person hands to somebody else, the install
# stick and the recovery ISO, built from a tree with no recovery key. A key
# built into them lets whoever holds its private half into their recovery
# system, and a release and a development build look the same from outside,
# so a release is refused while this tree has a key, when make reads this
# file and before anything is built. A key minted later in the same command
# is refused by tools/image/keyname.sh, since the names were chosen without
# it. RELEASE=1 asks the same of every goal but the three that remove things,
# and `make no-recovery-key` makes a tree that can build a release.
RELEASE_GOALS = rach2-install-stick rach2-recovery-iso
ifneq ($(filter release,$(MAKECMDGOALS))$(filter 1,$(RELEASE)),)
ifneq ($(filter-out clean distclean no-recovery-key,$(or $(MAKECMDGOALS),all)),)
ifneq ($(wildcard krffs/recovery_authorized_keys),)
$(error release: refused: krffs/recovery_authorized_keys is present, so every recovery artifact would let its key in; run make no-recovery-key first)
endif
endif
endif
.PHONY: release
release: $(RELEASE_GOALS)
	@echo "release: built: $(INSTALL_STICK) $(RECOVERY_ISO)"

DEMO_CREDS = krffs/authorized_keys krffs/shadow

IMAGE_TOOLS = krffs/mkimage.sh $(SETIDCHECK) tools/inspect/setid.allow \
              build/krffs/mkfs.krffs build/krffs/stageimage.krffs

-include $(TEST_IMAGE).d $(OS_SIMPLE_IMAGE).d $(OS_REAL_IMAGE).d

# The musl integration programs (tests/musl/*.c -> build/tests/musl/, or
# build/x86_64/tests/musl/), built by their own script for the architecture
# RACH2_ARCH names. They are end-to-end libc tests staged into the test image.
musl-programs: $(LIBC_STAMP)
	RACH2_ARCH=$(ARCH) sh tests/musl/build.sh

# The test image: the freestanding syscall probes plus the musl integration
# programs. The default suite boots it and judges each probe by its exit
# status; `make test` runs the style checks, the host tests and the core
# tier, which includes that suite.
$(TEST_IMAGE): $(DEMO_CREDS) krffs/manifests/test.manifest $(MANIFEST_DEPS_test) $(IMAGE_TOOLS) | preflight krffs rach2 tests musl-programs utils ish
	$(MKIMAGE) krffs/manifests/test.manifest $@ 10

# The os-simple image: a small Unix built out of this project's own code. init
# execs /bin/login, so this image needs the account databases;
# gen_demo_password.sh mints them when krffs/shadow is absent and does nothing
# when it is there, and it is the same file os-real stages, so one password
# opens either image.
$(OS_SIMPLE_IMAGE): $(DEMO_CREDS) krffs/manifests/os-simple.manifest $(MANIFEST_DEPS_os_simple) $(IMAGE_TOOLS) | preflight krffs rach2 ish utils
	$(MKIMAGE) krffs/manifests/os-simple.manifest $@ 10

# The os-real image: a real Unix-like environment (busybox shell + coreutils,
# dropbear SSH server and client, Doom, the manual, and an /etc + /root tree).
# 18 MiB on i686: the tree is under 16 MiB without the manual, and the
# manual's gzipped pages and alias table take under 1 MiB more. 20 MiB on
# x86-64, whose programs are larger: the tree with Doom's WAD takes 18.2 MiB.
# The module sits in the kernel's 32 MiB window, where the DMA buffers and the
# kernel stacks are carved, so it is kept small; and the manual's aliases are
# a table and not files, since the RAM root and a KRFFS disk walk every node
# on a lookup and four hundred alias files made the self-build suite five
# times slower. gen_demo_key.sh provisions the SSH login keys and
# gen_demo_password.sh the account passwords and /etc/shadow, each when
# absent; the check targets ensure the busybox/dropbear binaries are real.
OS_REAL_MIB = $(if $(filter x86_64,$(ARCH)),20,18)
$(OS_REAL_IMAGE): $(DEMO_CREDS) krffs/manifests/os-real.manifest $(MANIFEST_DEPS_os_real) $(IMAGE_TOOLS) $(MAN_ALL) | preflight krffs rach2 utils check-busybox check-tcc check-pdpmake check-setid check-dropbearmulti check-e2fsck ports/doom
	$(MKIMAGE) krffs/manifests/os-real.manifest $@ $(OS_REAL_MIB)

# The same manifest as an ext2 volume, which the root `rach2.root=ext2`
# mounts: 1 GiB, sparse on
# the host. tools/image/mkext2.sh stages it through stageimage's tree mode,
# formats it with the host's mke2fs on the one profile the driver writes,
# sets the owners with debugfs, and passes it through e2fsck and the set-id
# guard's own ext2 reader before it is named. The prerequisites are the KRFFS
# image's, so the two are built from one tree.
$(OS_REAL_EXT2): $(DEMO_CREDS) krffs/manifests/os-real.manifest $(MANIFEST_DEPS_os_real) tools/image/mkext2.sh $(IMAGE_TOOLS) $(MAN_ALL) | preflight krffs rach2 utils check-busybox check-tcc check-pdpmake check-setid check-dropbearmulti check-e2fsck ports/doom
	sh tools/image/mkext2.sh krffs/manifests/os-real.manifest $@ 1024

# The metal image: what the Lenovo G50-70 boots from a USB stick. The module
# GRUB loads is the whole system, since nothing is mounted from a disk, so it
# has a shell, dropbear, the account files and the keys, and leaves out the
# compiler, /src, Doom and bootctl, which a machine with no writable disk
# cannot use. 4 MB: the binaries come to about 1.4 MB, and the
# rest is headroom under the kernel's 32 MB direct map. The metal kernel: the
# same sources built read-only at the disk, into its own object tree so the two
# kernels never share one. The switch is a build switch and not a boot flag
# because a boot flag is editable at the GRUB prompt, and what it protects is
# somebody else's Windows installation. check_diskpolicy.sh then reads the
# policy back off the artifact, since there is nothing to check for by absence:
# the commands are numbers and the functions that issue them are static.
METAL_KERNEL = $(ARCH_BUILD)/rach2-metal/kernel
$(METAL_KERNEL): ask-the-sub-make
	@$(MAKE) --no-print-directory -C rach2 COMPONENT=rach2-metal KEXTRA=-DRACH2_DISK_READONLY
# One invocation, in the file rule above; see the note on `rach2-recovery`.
rach2-metal: krffs $(METAL_KERNEL)
	sh tools/inspect/check_diskpolicy.sh $(METAL_KERNEL) read-only

# The recovery kernel: the same sources, built with the authority to write any
# sector of the disk, into its own object tree so the two kernels never share
# one. A build switch and not a boot flag, for the reason above: a flag is
# editable at the GRUB prompt, and what this one guards is the whole disk rather
# than one file. It is NOT the read-only switch and must not be confused with
# it; that one is enforced below devfs and would stop this kernel writing the
# boot partition too, which recovery needs to do. check_recovery.sh reads the
# authority back off the artifact in both directions.
RECOVERY_KERNEL = $(ARCH_BUILD)/rach2-recovery/kernel
$(RECOVERY_KERNEL): ask-the-sub-make
	@$(MAKE) --no-print-directory -C rach2 COMPONENT=rach2-recovery KEXTRA=-DRACH2_RECOVERY
# The file rule above is the only place this kernel is built. Running the
# sub-make here as well gave two invocations with no edge between them, and a
# parallel build could reach both: they write the same object files, so one
# could link while the other rewrote an object under it.
rach2-recovery: krffs rach2 $(RECOVERY_KERNEL)
	sh tools/inspect/check_recovery.sh $(RECOVERY_KERNEL) recovery
	sh tools/inspect/check_recovery.sh $(KERNEL) ordinary
	sh tools/inspect/check_diskpolicy.sh $(RECOVERY_KERNEL) read-write

# A real file target with the components as order-only prerequisites, the shape
# $(TEST_IMAGE) above already uses. As a phony target this rebuilt the image on
# every invocation, so three suites that merely boot it each restaged it, and
# two of them running at once restaged it under each other.
$(METAL_IMAGE): $(DEMO_CREDS) krffs/manifests/rach2-metal.manifest $(MANIFEST_DEPS_rach2_metal) $(IMAGE_TOOLS) | preflight krffs rach2 rach2-metal utils check-busybox check-setid check-dropbearmulti
	$(MKIMAGE) krffs/manifests/rach2-metal.manifest $@ 4

rach2-metal-image: $(METAL_IMAGE)

# The recovery RAM image: what the recovery kernel loads as its root. It is the
# metal profile plus the programs that repair a disk and the pages that describe
# them, and it mounts nothing from the disk it is repairing, which is what lets
# it rewrite that disk's root.
#
# A recovery image that carries a key and one that carries none are identical
# from outside a booted machine, so the difference is in the name of every
# artifact that carries it: with a key the image is `rach2-recovery-with-key`,
# and so are the ISO, the install stick, the sealed disk and the GRUB disk
# built from it. A person about to write a stick reads the name of the file
# being written.
RECOVERY_NAME = rach2-recovery
RECOVERY_KEY_TAG =
ifneq ($(wildcard krffs/recovery_authorized_keys),)
RECOVERY_NAME = rach2-recovery-with-key
RECOVERY_KEY_TAG = -with-key
endif
RECOVERY_IMAGE = $(IMAGES_DIR)/$(RECOVERY_NAME).krffs
RECOVERY_ISO = $(IMAGES_DIR)/$(RECOVERY_NAME).iso
$(RECOVERY_IMAGE): $(DEMO_CREDS) krffs/manifests/rach2-recovery.manifest $(MANIFEST_DEPS_rach2_recovery) $(IMAGE_TOOLS) tools/image/keyname.sh | preflight krffs rach2 rach2-recovery utils check-busybox check-setid check-dropbearmulti check-e2fsck
	@sh tools/image/keyname.sh $@
	$(MKIMAGE) krffs/manifests/rach2-recovery.manifest $@ 4

rach2-recovery-image: $(RECOVERY_IMAGE)

# The os-real disk: the same os-real tree, but on a real disk image instead of a
# RAM-disk module. A 64 MB disk with a 48 MB KRFFS partition at LBA 2048; the
# partition is larger than the 8 MiB resident page-cache cap on purpose, so it
# can later prove an on-disk file system bigger than what fits in RAM at once.
os-real-disk: krffs rach2 utils check-busybox check-tcc check-pdpmake check-setid check-dropbearmulti check-e2fsck ports/doom $(MAN_ALL)
	$(MKDISK) krffs/manifests/os-real.manifest $(OS_REAL_DISK) 64 48

# `make rach2-sealed-disk`: the disk the installer writes to a machine and the
# machine boots from. Its geometry is a contract the builder checks against the
# built image: a 64 MiB FAT32 boot partition at 1 MiB, at 4 KiB clusters (one
# sector per cluster puts a 64 MiB volume past both of bootslot's bounds), a
# 64 MiB recovery partition after it whose kernel and RAM image GRUB reaches
# when partition 1 cannot be read at all, and the ext2 root, 1 GiB at 130 MiB,
# the same manifest as an ext2 volume, which the candidate and the fallback
# entries both boot. The image is 1155 MiB. Its recovery partition carries the
# recovery image, so a tree with a recovery key names it
# `rach2-sealed-with-key.disk`.
#
# The address is `rach2.ip=none`: this machine is carried between buildings and
# takes whatever the network there gives it, through `ifup -a` and udhcpc, which
# init runs before it starts the SSH server. A fixed address on the boot line
# would be one more thing to edit at the desk. The recovery image's
# `rescue-static` takes the fixed address below when no lease arrives, for the
# day the network has no server; the address, mask, prefix and gateway are
# overridable at build time for anywhere else, and the configuration checker
# holds the prefix, the mask and the script to one subnet with the gateway
# inside it before a disk is written, since a machine whose record and fallback
# disagree is one nobody can reach by either.
SEALED_ADDRESS ?= 192.168.0.105
SEALED_MASK ?= 255.255.255.0
SEALED_PREFIX ?= 24
SEALED_GATEWAY ?= 192.168.0.1
SEALED_DISK = $(IMAGES_DIR)/rach2-sealed$(RECOVERY_KEY_TAG).disk

# The file systems in files a fresh disk carries in /demo, one of each kind
# this kernel mounts, each with a hello.txt in it, and an empty KRFFS volume
# to copy when a person wants an image of their own. They are built here
# rather than staged from somewhere, because what they are is what the host's
# own formatters write.
FS_DEMO_DIR = $(IMAGES_DIR)/fs-demo
FS_DEMO_IMAGES = $(FS_DEMO_DIR)/fat32.img $(FS_DEMO_DIR)/exfat.img \
                 $(FS_DEMO_DIR)/ext2.img $(FS_DEMO_DIR)/krffs.img \
                 $(FS_DEMO_DIR)/empty.krffs

$(FS_DEMO_DIR)/fat32.img: krffs/demo/hello.txt
	@mkdir -p $(FS_DEMO_DIR)
	dd if=/dev/zero of=$@.new bs=1048576 count=4 2>/dev/null
	mformat -i $@.new -F -T 8192 -v RACHDEMO ::
	mcopy -i $@.new krffs/demo/hello.txt ::/hello.txt
	mv $@.new $@

$(FS_DEMO_DIR)/exfat.img: build/tests/host/mkexfatfixture
	@mkdir -p $(FS_DEMO_DIR)
	build/tests/host/mkexfatfixture $@.new --demo
	build/tests/host/mkexfatfixture --check $@.new > /dev/null
	mv $@.new $@

$(FS_DEMO_DIR)/ext2.img: krffs/manifests/fs-demo.manifest krffs/demo/hello.txt \
                         tools/image/mkext2.sh $(IMAGE_TOOLS) | krffs
	@mkdir -p $(FS_DEMO_DIR)
	sh tools/image/mkext2.sh krffs/manifests/fs-demo.manifest $@ 4

$(FS_DEMO_DIR)/krffs.img: krffs/manifests/fs-demo.manifest krffs/demo/hello.txt \
                          krffs/mkimage.sh | krffs
	@mkdir -p $(FS_DEMO_DIR)
	sh krffs/mkimage.sh krffs/manifests/fs-demo.manifest $@ 4

# Formatted and nothing else: what a person copies to make an image of their
# own, since the machine has no formatter of its own.
$(FS_DEMO_DIR)/empty.krffs: krffs/mkfs.krffs.c | krffs
	@mkdir -p $(FS_DEMO_DIR)
	dd if=/dev/zero of=$@.new bs=1048576 count=4 2>/dev/null
	build/krffs/mkfs.krffs $@.new > /dev/null
	build/krffs/fsck.krffs $@.new --strict > /dev/null
	mv $@.new $@

fs-demo-images: $(FS_DEMO_IMAGES)
.PHONY: fs-demo-images

# The host tools the disk and stick scripts run, named here because the rules
# below list them as prerequisites and make expands a prerequisite list as it
# reads it. What builds them is in mk/images.mk, which is included at the end
# of this file, after the names the rules there need.
MKMBR           = build/tools/image/mkmbr
MKGPT           = build/tools/image/mkgpt
FATCHAINS       = build/tools/image/fatchains
GRUBEMBED       = build/tools/image/grubembed
STICKCHECK      = build/tools/image/stickcheck
DISK_TOOLS      = $(MKMBR) $(MKGPT) $(FATCHAINS) $(GRUBEMBED)
# The pinned GRUB images and their manifest, which the disk and ISO scripts
# copy and check, so that a regenerated image rebuilds what carries it.
GRUB_FILES      = $(wildcard vendor/grub/*.img) vendor/grub/bootx64.efi \
                  vendor/grub/manifest.txt vendor/grub/verify_grub.sh
MKEXFATFIXTURE  = build/tests/host/mkexfatfixture

SEALED_EXT2 = $(IMAGES_DIR)/rach2-metal-disk.ext2
$(SEALED_EXT2): $(FS_DEMO_IMAGES) $(DEMO_CREDS) krffs/manifests/rach2-metal-disk.manifest $(MANIFEST_DEPS_rach2_metal_disk) $(MAN_ALL) tools/image/mkext2.sh $(IMAGE_TOOLS) | preflight krffs rach2 utils check-busybox check-tcc check-pdpmake check-setid check-dropbearmulti ports/doom
	sh tools/image/mkext2.sh krffs/manifests/rach2-metal-disk.manifest $@ 1024
# The alias builds everything the disk is made of, then asks for the disk
# itself. The two are separate because the components are phony targets, which
# are always out of date, and a disk that named them directly was rebuilt on
# every invocation: half a minute, on a machine that had not changed.
rach2-sealed-disk: krffs rach2 utils rach2-recovery rach2-recovery-image check-busybox check-tcc check-pdpmake check-setid check-dropbearmulti ports/doom $(CFGCHECK)
	@$(MAKE) --no-print-directory $(SEALED_DISK)

$(SEALED_DISK): $(KERNEL) $(RECOVERY_KERNEL) $(RECOVERY_IMAGE) $(SEALED_EXT2) \
                krffs/manifests/rach2-metal-disk.manifest krffs/rescue-static.script \
                tools/image/mkdisk_grub.sh tools/image/keyname.sh $(DISK_TOOLS) $(GRUB_FILES) \
                $(CFGCHECK)
	@sh tools/image/keyname.sh $@
	@$(CFGCHECK) sealednet $(SEALED_ADDRESS) $(SEALED_PREFIX) \
	    $(SEALED_MASK) $(SEALED_GATEWAY) krffs/rescue-static.script
	RACH2_DISK_BOOT_MB=64 \
	RACH2_DISK_CLUSTER=8 \
	RACH2_DISK_RECOVERY_MB=64 \
	RACH2_DISK_APPEND="rach2.disk=ahci rach2.netconsole" \
	RACH2_DISK_DHCP_APPEND="rach2.ip=none" \
	RACH2_DISK_RECOVERY_KERNEL=$(RECOVERY_KERNEL) \
	RACH2_DISK_RECOVERY_IMAGE=$(RECOVERY_IMAGE) \
	RACH2_DISK_EXT2_MB=1024 \
	RACH2_DISK_EXT2_IMAGE=$(SEALED_EXT2) \
	RACH2_DISK_HELLO=krffs/demo/hello.txt \
	    sh tools/image/mkdisk_grub.sh $(KERNEL) $(SEALED_DISK)



# The GRUB disk `rach2/run_from_disk.sh` boots: the os-real system as an ext2
# root on the third partition, reached through the boot loader with no kernel
# given to the emulator, which is the path a machine takes. The suites build
# their own disks in tests/system/suites.mk; this one exists for a person, and
# no suite boots it, so tests/system/build_rules_test.sh reads its rule from
# the make database instead. Its recovery partition carries the recovery
# image, so a tree with a recovery key names it `os-real-grub-with-key.disk`.
.PHONY: os-real-grub-disk
os-real-grub-disk: krffs rach2 utils rach2-recovery rach2-recovery-image check-busybox check-tcc check-pdpmake check-setid check-dropbearmulti check-e2fsck ports/doom
	@$(MAKE) --no-print-directory $(OS_REAL_GRUB_DISK)

$(OS_REAL_GRUB_DISK): $(KERNEL) $(RECOVERY_KERNEL) $(RECOVERY_IMAGE) $(OS_REAL_EXT2) \
                      krffs/manifests/os-real.manifest tools/image/mkdisk_grub.sh \
                      tools/image/keyname.sh $(DISK_TOOLS) $(GRUB_FILES)
	@sh tools/image/keyname.sh $@
	RACH2_DISK_BOOT_MB=64 \
	RACH2_DISK_CLUSTER=8 \
	RACH2_DISK_RECOVERY_MB=64 \
	RACH2_DISK_RECOVERY_KERNEL=$(RECOVERY_KERNEL) \
	RACH2_DISK_RECOVERY_IMAGE=$(RECOVERY_IMAGE) \
	RACH2_DISK_EXT2_MB=1024 \
	RACH2_DISK_EXT2_IMAGE=$(OS_REAL_EXT2) \
	    sh tools/image/mkdisk_grub.sh $(KERNEL) $(OS_REAL_GRUB_DISK)


# The public test entry: the source checks, the native tests, and the core
# tier, in that order. The first two share a scratch directory, so the runner
# runs them alone and before the pool: the style suite builds the kernel that
# its warning check judges, and the host suite builds the native test programs
# with its own sanitizer flags. The tier is the system suites that between
# them exercise every subsystem. The runner owns the registry, so this holds
# no suite knowledge of its own.
# The slow analysers (tools/verify/analyze.sh): -fanalyzer, the exhaustive
# cppcheck, clang-tidy and the linker's dead-code report. Run at a
# milestone's close; the style suite keeps to the fast checks.
.PHONY: analyze
analyze:
	sh tools/verify/analyze.sh

test:
	@./tests/run style
	@./tests/run host
	@./tests/run core

# Bootable ISO images for real hardware: each embeds the kernel and one RAM-disk
# image as GRUB multiboot modules, assembled natively with xorriso and the
# pinned GRUB El Torito images (see tools/image/mkiso.sh). Output:
# build/images/<image>.iso.
isos: test-iso os-simple-iso os-real-iso rach2-metal-iso rach2-recovery-iso
test-iso: test-image
	RACH2_ARCH=$(ARCH) sh tools/image/mkiso.sh test
os-simple-iso: os-simple-image
	RACH2_ARCH=$(ARCH) sh tools/image/mkiso.sh os-simple
os-real-iso: os-real-image
	RACH2_ARCH=$(ARCH) sh tools/image/mkiso.sh os-real
rach2-metal-iso: rach2-metal rach2-metal-image $(IMAGES_DIR)/rach2-metal.iso
rach2-recovery-iso: rach2-recovery rach2-recovery-image $(RECOVERY_ISO)

# The staged capstone binaries are the REAL prerequisites of the os-real image.
# A stamp only proves its build RAN; the artifact can be deleted OR truncated
# afterwards while the stamp lingers, so these PHONY check targets (depending on
# the stamp so it builds first) run `test -s` on EVERY build and catch a missing
# OR zero-byte artifact. Both binaries, both applet tables and both resolved
# configs, because a stamp file outlives an artifact somebody deleted or
# truncated, and the set-id ones are the artifacts whose absence would be
# noticed last and matter most. The compiler the guest runs, and the two things
# it cannot work without: its runtime and its own headers. The ELF check is the
# one that matters: `file(1)` would call a Mach-O or an arm64 build "an
# executable" and this kernel would refuse it, and a dynamically linked one asks
# for a loader the musl roots do not have, so exec would refuse it too. The
# last check reads
# the output policy's own diagnostics out of the shipped binary, since a patch
# that did not apply and a compiler that accepts `-run` look identical from
# outside.
check-pdpmake: $(PDPMAKE_STAMP) $(PDPMAKE_BIN) $(ELFCHECK)
	@test -s $(PDPMAKE_BIN) || { \
	    echo "pdpmake: $(PDPMAKE_BIN) missing or empty; rm $(PDPMAKE_STAMP) and rebuild (make pdpmake)" >&2; \
	    exit 1; }
	@$(ELFCHECK) --arch $(ARCH) $(PDPMAKE_BIN)

# The port's size limits, each set from its first build with a tenth of
# headroom, e2fsck's in milestone 42 at 673672 bytes stripped,
# resize2fs's in milestone 47, mke2fs's in milestone 55 at 501608 and
# tune2fs's and dumpe2fs's in the same at 482664 and 389896: a configure
# that let a library or a program back in would be caught here and not in an
# image. The limits serve both architectures: the first x86-64 build, in
# milestone 57, came in under the i686 sizes, e2fsck at 649848 bytes.
E2FSCK_LIMIT = 737280
RESIZE2FS_LIMIT = 430080
MKE2FS_LIMIT = 552960
TUNE2FS_LIMIT = 532480
DUMPE2FS_LIMIT = 430080
check-e2fsck: $(E2FSPROGS_STAMP) $(E2FSCK_BIN) $(RESIZE2FS_BIN) $(MKE2FS_BIN) $(TUNE2FS_BIN) \
              $(DUMPE2FS_BIN) $(ELFCHECK)
	@for f in $(E2FSCK_BIN) $(RESIZE2FS_BIN) $(MKE2FS_BIN) $(TUNE2FS_BIN) $(DUMPE2FS_BIN); do test -s $$f || { \
	    echo "e2fsprogs: $$f missing or empty; rm $(E2FSPROGS_STAMP) and rebuild (make e2fsprogs)" >&2; \
	    exit 1; }; done
	@$(ELFCHECK) --arch $(ARCH) $(E2FSCK_BIN)
	@$(ELFCHECK) --arch $(ARCH) $(RESIZE2FS_BIN)
	@$(ELFCHECK) --arch $(ARCH) $(MKE2FS_BIN)
	@$(ELFCHECK) --arch $(ARCH) $(TUNE2FS_BIN)
	@$(ELFCHECK) --arch $(ARCH) $(DUMPE2FS_BIN)
	@size=$$(wc -c < $(E2FSCK_BIN) | tr -d ' '); test "$$size" -le $(E2FSCK_LIMIT) || { \
	    echo "e2fsprogs: e2fsck is $$size bytes, over the $(E2FSCK_LIMIT) limit vendor/e2fsprogs/Readme.md sets" >&2; \
	    exit 1; }
	@size=$$(wc -c < $(RESIZE2FS_BIN) | tr -d ' '); test "$$size" -le $(RESIZE2FS_LIMIT) || { \
	    echo "e2fsprogs: resize2fs is $$size bytes, over the $(RESIZE2FS_LIMIT) limit vendor/e2fsprogs/Readme.md sets" >&2; \
	    exit 1; }
	@size=$$(wc -c < $(MKE2FS_BIN) | tr -d ' '); test "$$size" -le $(MKE2FS_LIMIT) || { \
	    echo "e2fsprogs: mke2fs is $$size bytes, over the $(MKE2FS_LIMIT) limit vendor/e2fsprogs/Readme.md sets" >&2; \
	    exit 1; }
	@size=$$(wc -c < $(TUNE2FS_BIN) | tr -d ' '); test "$$size" -le $(TUNE2FS_LIMIT) || { \
	    echo "e2fsprogs: tune2fs is $$size bytes, over the $(TUNE2FS_LIMIT) limit vendor/e2fsprogs/Readme.md sets" >&2; \
	    exit 1; }
	@size=$$(wc -c < $(DUMPE2FS_BIN) | tr -d ' '); test "$$size" -le $(DUMPE2FS_LIMIT) || { \
	    echo "e2fsprogs: dumpe2fs is $$size bytes, over the $(DUMPE2FS_LIMIT) limit vendor/e2fsprogs/Readme.md sets" >&2; \
	    exit 1; }

check-tcc: $(TCC_STAMP) $(TCC_OUTPUTS) $(BINFIND) $(ELFCHECK)
	@for f in $(TCC_BIN) $(TCC_STAGE)/libtcc1.a \
	          $(TCC_STAGE)/include/stdarg.h \
	          $(TCC_STAGE)/include/stddef.h \
	          $(TCC_STAGE)/include/stdbool.h \
	          $(TCC_STAGE)/include/float.h \
	          $(TCC_STAGE)/include/varargs.h; do \
	    test -s "$$f" || { echo "tcc: $$f missing or empty; rm $(TCC_STAMP) and rebuild (make tcc)" >&2; exit 1; }; \
	done
	@$(ELFCHECK) --selftest
	@$(ELFCHECK) --arch $(ARCH) $(TCC_BIN)
	@for m in "must be linked with -static" "run is not available" \
	          "shared is not available" "must be an ELF"; do \
	    $(BINFIND) "$$m" $(TCC_BIN) || { echo "tcc: the shipped compiler does not carry the output policy ($$m); rm $(TCC_STAMP) and rebuild" >&2; exit 1; }; \
	done

check-busybox: $(BUSYBOX_STAMP) $(BUSYBOX_OUTPUTS)
	@for f in $(BUSYBOX_BIN) $(BUSYBOX_SUID_BIN) \
	          $(BUSYBOX_STAGE)/applet_tables-main.h \
	          $(BUSYBOX_STAGE)/applet_tables-suid.h \
	          $(BUSYBOX_STAGE)/config-main \
	          $(BUSYBOX_STAGE)/config-suid; do \
	    test -s "$$f" || { echo "busybox: $$f missing or empty; rm $(BUSYBOX_STAMP) and rebuild (make busybox)" >&2; exit 1; }; \
	done

# Privilege climbs through a declared set of paths and no others. The sweep
# reads every manifest against a per-image allowlist, so a set-id line anywhere,
# under any of the six mode-bearing directives, fails the build rather than a
# boot. Every artifact a check reads becomes a prerequisite here, in the phase
# that adds the check, because make does not order the siblings of a
# prerequisite list. The manifest sweep needs no build product, but the
# applet-table check reads one, so this target is ordered against the busybox
# stamp.
# Unordered it would fail a correct tree on a header that does not exist yet,
# or, in the case that matters, read the PREVIOUS configuration's table after
# rach2.config was edited, pass, and let the image stage the busybox whose
# re-enabled applet the check exists to forbid: build_busybox.sh opens by
# removing and re-extracting the tree, so for most of a rebuild the header is
# absent and
# before that it is stale. The applet half reads two generated applet tables and
# the init half reads the three init binaries the allowlist names, so both are
# ordered after what produces them, the way phase 3's rule requires: unordered,
# a parallel build scans a build/utils/sbin/ that holds yesterday's artifacts
# and passes while today's are staged.
check-setid: $(BUSYBOX_STAMP) utils $(SETIDCHECK)
	@$(SETIDCHECK)
	@$(SETIDCHECK) --applets
check-dropbearmulti: $(DROPBEAR_STAMP) $(DROPBEAR_BIN)
	@test -s $(DROPBEAR_BIN) || { echo "dropbear: $(DROPBEAR_BIN) missing or empty; rm $(DROPBEAR_STAMP) and rebuild (make dropbear)" >&2; exit 1; }

# musl's stamp guards two artifacts, not one: the installed libc the cross links
# against, and the EXTRACTED source tree, which krffs (mkhash's crypt) and the
# mkhash host test compile from directly. A copied worktree carries the stamp
# without either, so check both here rather than let a consumer fail later on a
# missing file with no cause named.
check-libc: $(LIBC_STAMP) $(MUSL_OUTPUTS)
	@test -s $(LIBC_A) || { echo "libc: $(LIBC_A) missing or empty; rm $(LIBC_STAMP) and rebuild (make)" >&2; exit 1; }
	@test -f $(MUSL_CRYPT_SRC) || { echo "libc: $(MUSL_CRYPT_SRC) missing; rm $(LIBC_STAMP) and rebuild (make)" >&2; exit 1; }

# The test suites: every `run-NAME` target, the fixtures they write, the nine
# test tools, and `prepare-all`. Included rather than kept apart so that one
# dependency graph covers the product and the variants only a suite needs.
include mk/host.mk
include mk/images.mk
include mk/gnu.mk
include mk/gnu-extra.mk
include mk/gnu-image.mk
include tests/system/suites.mk

# The host programs the build itself needs, for the compilation database
# generator, which dry-runs their rules; named after the includes that define
# them. The KRFFS programs and the native host tests have their own rules and
# the generator collects them separately.
HOST_TOOLS = $(BINFIND) $(ELFCHECK) $(SETIDCHECK) $(CFGCHECK) $(PROSECHECK) $(GUESTCHECK) $(SIZECHECK) \
             $(SHA256TOOL) $(MANCHECK) $(MKMBR) $(FATCHAINS) $(GRUBEMBED) $(STICKCHECK) \
             $(MKEXFATFIXTURE) $(DEADLINE) $(PCAPCHECK) $(MKFIXTURE) $(MKFATFIXTURE) \
             $(HOSTPEER) $(WAVCHECK) $(HARNESS) $(RUNTESTS) $(PTYDRIVE) $(RUNUTIL) \
             $(MKGPT) $(GROWSET) $(ASCIISCAN) $(HOSTCHECK) $(MACHINENET)
.PHONY: host-tools print-host-tools
host-tools: $(HOST_TOOLS)
print-host-tools:
	@echo $(HOST_TOOLS)

# Fail if a rach2 .c on disk is missing from C_SRC, so a new kernel source
# cannot be dropped from the build without anybody noticing. `tests/run style`
# runs this.
check-sources:
	@./tools/verify/check_sources.sh

# clean: the everyday reset. Each sub-make removes its own build/<component>
# tree (objects, .d, binaries), and the RAM-disk images under build/images go
# too. What no sub-make owns stays: the host tools under build/tools, the
# manual pages under build/man, and the variant kernel directories a suite
# builds. The expensive vendored third-party is LEFT in place:
# vendor/musl/install,
# the extracted busybox/dropbear trees, and the staged capstones under
# build/vendor (which no sub-make owns). Use distclean for those.
clean:
	@for dir in $(BASE_DIRS) $(FULL_DIRS); do $(MAKE) -C $$dir clean; done
	rm -rf $(IMAGES_DIR) $(ARCH_BUILD)/iso

# distclean: a full from-scratch reset; clean plus every third-party build
# product: the stamps, vendor/musl/install, the extracted vendored source trees,
# and the entire out-of-tree build/ (including the staged capstones under
# build/vendor that a routine clean leaves in place, and everything of x86-64,
# whose stamps and extracted trees are under build/x86_64/vendor-work). The
# next build rebuilds musl/busybox/dropbear/tcc/pdpmake from the tarballs. The
# stamps go with the trees they stand for: a stamp left behind while its binary
# went with build/ would say work was done that was not. The parse-time rules
# above retire such a stamp when they find its output gone, so the two paths
# agree.
distclean: clean
	rm -f vendor/musl/.stamp vendor/busybox/.stamp vendor/dropbear/.stamp vendor/tcc/.stamp \
	      vendor/pdpmake/.stamp vendor/e2fsprogs/.stamp vendor/musl/.outputs vendor/tcc/.outputs \
	      vendor/tcc/.validated vendor/pdpmake/.validated
	rm -rf vendor/musl/install \
	       $(LIBC_TARBALL:.tar.gz=) $(BUSYBOX_TARBALL:.tar.bz2=) \
	       $(DROPBEAR_TARBALL:.tar.bz2=) $(TCC_TARBALL:.tar.bz2=) \
	       $(PDPMAKE_TARBALL:.tgz=) $(E2FSPROGS_TARBALL:.tar.gz=)
	rm -rf build


