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
#   make test       boot the test image and check every probe passes.
#   tests/run NAME  run one named suite (style, host, ssh, all, ...).
#   make isos       a bootable ISO per image, for real hardware (needs xorriso).
#   make clean      remove the in-house build products and the images
#   make distclean  + the vendored musl, busybox, dropbear, tcc and pdpmake
#                   builds (full from-scratch)

# Run a built image with rach2/run.sh <test|os-simple|os-real>; the build system
# does not run QEMU (that script models the disk/audio/network/-cpu matrix).

# --- components (each builds via its own sub-make) ---
BASE_DIRS = krffs rach2 tests ish utils
FULL_DIRS = ports/doom

# --- vendored third-party: built by their own scripts (which wrap musl's
#     autotools, busybox/dropbear's kbuild, tcc's hand-written configure and
#     pdpmake's bare POSIX makefile), tracked with stamp files so a clean
#     clone "just works" without running the build_*.sh by hand, and each
#     rebuilds only when its real inputs (tarball, build script, config)
#     change. ---
LIBC_STAMP     = vendor/musl/.stamp
BUSYBOX_STAMP  = vendor/busybox/.stamp
DROPBEAR_STAMP = vendor/dropbear/.stamp
TCC_STAMP      = vendor/tcc/.stamp
PDPMAKE_STAMP  = vendor/pdpmake/.stamp
E2FSPROGS_STAMP = vendor/e2fsprogs/.stamp

# The staged capstone binaries the build_*.sh scripts produce, out of tree under
# build/vendor/ (mirroring their vendor/<name>/ sources). A routine `make clean`
# never reaches build/vendor, so these expensive artifacts survive it; only
# `make distclean` removes them. They are the REAL prerequisites of the capstone
# image (see the check targets below): a stamp only proves its build RAN.
BUSYBOX_BIN  = build/vendor/busybox/busybox
BUSYBOX_SUID_BIN = build/vendor/busybox/busybox-suid
DROPBEAR_BIN = build/vendor/dropbear/dropbearmulti
TCC_BIN      = build/vendor/tcc/tcc
PDPMAKE_BIN  = build/vendor/pdpmake/make
E2FSCK_BIN   = build/vendor/e2fsprogs/e2fsck
RESIZE2FS_BIN = build/vendor/e2fsprogs/resize2fs

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

# The two musl artifacts consumers depend on, both derived from the tarball name
# so the pinned version is written once: the installed static libc the
# cross-compiled userland links against, and the extracted source, which the
# host-compiled mkhash and its host test read the SHA-512 crypt from.
LIBC_A         = vendor/musl/install/lib/libc.a
MUSL_CRYPT_SRC = $(LIBC_TARBALL:.tar.gz=)/src/crypt/crypt_sha512.c

IMAGES_DIR      = build/images
TEST_IMAGE      = $(IMAGES_DIR)/test.krffs
OS_SIMPLE_IMAGE = $(IMAGES_DIR)/os-simple.krffs
OS_REAL_IMAGE   = $(IMAGES_DIR)/os-real.krffs
OS_REAL_EXT2    = $(IMAGES_DIR)/os-real.ext2
METAL_IMAGE     = $(IMAGES_DIR)/rach2-metal.krffs
OS_REAL_DISK    = $(IMAGES_DIR)/os-real.disk
MKIMAGE         = krffs/mkimage.sh
BINFIND         = build/tools/inspect/binfind
ELFCHECK        = build/tools/inspect/elfcheck
SETIDCHECK      = build/tools/inspect/setidcheck
CFGCHECK        = build/tools/inspect/cfgcheck
PROSECHECK      = build/tools/verify/prosecheck
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
# images, and the validators that judge them. It builds no test tool. The tools
# are the means of checking the product, they live in tests/system/suites.mk,
# and each suite target builds the ones it invokes.
all: images $(BINFIND) $(ELFCHECK) $(SHA256TOOL)

# The cached tool check (tools/setup/preflight.sh): a missing or changed build
# tool fails here, named, before any compilation. It is an order-only
# prerequisite of the build entry points; clean and distclean never run it.
preflight:
	@./tools/setup/preflight.sh

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

$(LIBC_STAMP): vendor/musl/build_libc.sh $(LIBC_TARBALL) $(LIBC_PATCHES)
	@rm -f $@ vendor/musl/install/lib/libc.a
	+@if [ -n "$(findstring n,$(firstword $(MAKEFLAGS)))" ]; then echo "$@: dry run"; else t0=$$(date +%s); ./vendor/musl/build_libc.sh && echo "libc: result: built in $$(($$(date +%s)-t0))s"; fi
	@test -s vendor/musl/install/lib/libc.a || { echo "libc: build produced no libc.a" >&2; exit 1; }
	@{ find vendor/musl/install -type f | sort; echo $(MUSL_CRYPT_SRC); } > vendor/musl/.outputs
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
                  vendor/busybox/rach2-suid.config vendor/busybox/muslgcc \
                  $(BUSYBOX_TARBALL) $(BUSYBOX_PATCHES) $(LIBC_STAMP)
	@rm -f $@ $(BUSYBOX_BIN) $(BUSYBOX_SUID_BIN) \
	    build/vendor/busybox/applet_tables-main.h \
	    build/vendor/busybox/applet_tables-suid.h \
	    build/vendor/busybox/config-main \
	    build/vendor/busybox/config-suid
	+@if [ -n "$(findstring n,$(firstword $(MAKEFLAGS)))" ]; then echo "$@: dry run"; else t0=$$(date +%s); ./vendor/busybox/build_busybox.sh && echo "busybox: result: built in $$(($$(date +%s)-t0))s"; fi
	@test -s $(BUSYBOX_BIN) || { echo "busybox: build produced no $(BUSYBOX_BIN)" >&2; exit 1; }
	@touch $@

$(TCC_STAMP): vendor/tcc/build_tcc.sh vendor/tcc/rach2-output-policy.patch \
              vendor/busybox/muslgcc $(TCC_TARBALL) $(LIBC_STAMP)
	@rm -f $@ $(TCC_BIN) build/vendor/tcc/libtcc1.a \
	    build/vendor/tcc/include/stdarg.h
	+@if [ -n "$(findstring n,$(firstword $(MAKEFLAGS)))" ]; then echo "$@: dry run"; else t0=$$(date +%s); ./vendor/tcc/build_tcc.sh && echo "tcc: result: built in $$(($$(date +%s)-t0))s"; fi
	@test -s $(TCC_BIN) || { echo "tcc: build produced no $(TCC_BIN)" >&2; exit 1; }
	@find build/vendor/tcc -type f | sort > vendor/tcc/.outputs
	@touch $@

# pdpmake, the make that runs ON the machine. Its patches are prerequisites for
# the reason musl's are: they are applied at extraction, so a stamp that
# predates one would keep a make whose built-in rules name a compiler this image
# does not have and whose default link this system's compiler refuses.
$(PDPMAKE_STAMP): vendor/pdpmake/build_pdpmake.sh $(PDPMAKE_PATCHES) \
                  vendor/busybox/muslgcc $(PDPMAKE_TARBALL) $(LIBC_STAMP)
	@rm -f $@ $(PDPMAKE_BIN)
	+@if [ -n "$(findstring n,$(firstword $(MAKEFLAGS)))" ]; then echo "$@: dry run"; else t0=$$(date +%s); ./vendor/pdpmake/build_pdpmake.sh && echo "pdpmake: result: built in $$(($$(date +%s)-t0))s"; fi
	@test -s $(PDPMAKE_BIN) || { echo "pdpmake: build produced no $(PDPMAKE_BIN)" >&2; exit 1; }
	@touch $@

# e2fsprogs, of which the recovery image stages e2fsck alone. The patch
# manifest is a prerequisite as Dropbear's is below.
$(E2FSPROGS_STAMP): vendor/e2fsprogs/build_e2fsprogs.sh vendor/e2fsprogs/muslgcc \
                    $(E2FSPROGS_PATCHES) $(E2FSPROGS_TARBALL) $(LIBC_STAMP)
	@rm -f $@ $(E2FSCK_BIN) $(RESIZE2FS_BIN)
	+@if [ -n "$(findstring n,$(firstword $(MAKEFLAGS)))" ]; then echo "$@: dry run"; else t0=$$(date +%s); ./vendor/e2fsprogs/build_e2fsprogs.sh && echo "e2fsprogs: result: built in $$(($$(date +%s)-t0))s"; fi
	@test -s $(E2FSCK_BIN) || { echo "e2fsprogs: build produced no $(E2FSCK_BIN)" >&2; exit 1; }
	@test -s $(RESIZE2FS_BIN) || { echo "e2fsprogs: build produced no $(RESIZE2FS_BIN)" >&2; exit 1; }
	@touch $@

# The patch manifest is a prerequisite for the reason musl's is: the script
# applies what patches/series names, so a series edit or a patch edit must
# invalidate the stamp.
DROPBEAR_PATCHES = vendor/dropbear/patches/series $(wildcard vendor/dropbear/patches/*.patch)

$(DROPBEAR_STAMP): vendor/dropbear/build_dropbear.sh vendor/dropbear/localoptions.h vendor/dropbear/muslgcc \
                   $(DROPBEAR_PATCHES) $(DROPBEAR_TARBALL) $(LIBC_STAMP)
	@rm -f $@ $(DROPBEAR_BIN)
	+@if [ -n "$(findstring n,$(firstword $(MAKEFLAGS)))" ]; then echo "$@: dry run"; else t0=$$(date +%s); ./vendor/dropbear/build_dropbear.sh && echo "dropbear: result: built in $$(($$(date +%s)-t0))s"; fi
	@test -s $(DROPBEAR_BIN) || { echo "dropbear: build produced no $(DROPBEAR_BIN)" >&2; exit 1; }
	@touch $@

# --- vendor output healing. The stamps prove the scripts ran; the files the
#     scripts staged are what the graph consumes. A deleted output retires its
#     vendor's stamp HERE, at parse time, so the ordinary stamp rule rebuilds
#     the vendor and every transitive dependent in the same run; a recipe-time
#     sub-make could not, because make's cached timestamps would miss the
#     stamp's change. The dependency-only rules below make each output a known
#     target ordered behind its stamp. A truncated output is the validators'
#     business, not this rule's. musl and TinyCC declare their whole staged
#     trees in .outputs lists their stamp recipes write. ---
MUSL_OUTPUTS = $(shell cat vendor/musl/.outputs 2>/dev/null)
TCC_OUTPUTS = $(shell cat vendor/tcc/.outputs 2>/dev/null)
BUSYBOX_OUTPUTS = $(BUSYBOX_BIN) $(BUSYBOX_SUID_BIN) \
                  build/vendor/busybox/applet_tables-main.h \
                  build/vendor/busybox/applet_tables-suid.h \
                  build/vendor/busybox/config-main \
                  build/vendor/busybox/config-suid

_ := $(shell [ ! -f $(LIBC_STAMP) ] || [ ! -f vendor/musl/.outputs ] || \
        while IFS= read -r f; do [ -e "$$f" ] || { rm -f $(LIBC_STAMP); break; }; done \
            < vendor/musl/.outputs)
_ := $(shell [ ! -f $(TCC_STAMP) ] || [ ! -f vendor/tcc/.outputs ] || \
        while IFS= read -r f; do [ -e "$$f" ] || { rm -f $(TCC_STAMP); break; }; done \
            < vendor/tcc/.outputs)
_ := $(shell [ ! -f $(BUSYBOX_STAMP) ] || \
        for f in $(BUSYBOX_OUTPUTS); do [ -e "$$f" ] || { rm -f $(BUSYBOX_STAMP); break; }; done)
_ := $(shell [ ! -f $(DROPBEAR_STAMP) ] || [ -s $(DROPBEAR_BIN) ] || rm -f $(DROPBEAR_STAMP))
_ := $(shell [ ! -f $(PDPMAKE_STAMP) ] || [ -s $(PDPMAKE_BIN) ] || rm -f $(PDPMAKE_STAMP))
_ := $(shell [ ! -f $(E2FSPROGS_STAMP) ] || { [ -s $(E2FSCK_BIN) ] && [ -s $(RESIZE2FS_BIN) ]; } || rm -f $(E2FSPROGS_STAMP))

# The tree lists above test existence only, because musl installs several
# legitimately empty headers. The binaries that can never be empty also get a
# size test, so a truncated one rebuilds its vendor instead of failing a check
# with advice.
_ := $(shell [ ! -f $(LIBC_STAMP) ] || [ -s vendor/musl/install/lib/libc.a ] || rm -f $(LIBC_STAMP))
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
MANIFEST_DEPS_test := $(shell sh krffs/manifest_deps.sh krffs/manifests/test.manifest)
MANIFEST_DEPS_os_simple := $(shell sh krffs/manifest_deps.sh krffs/manifests/os-simple.manifest)
MANIFEST_DEPS_os_real := $(shell sh krffs/manifest_deps.sh krffs/manifests/os-real.manifest)
MANIFEST_DEPS_rach2_metal := $(shell sh krffs/manifest_deps.sh krffs/manifests/rach2-metal.manifest)
MANIFEST_DEPS_rach2_recovery := $(shell sh krffs/manifest_deps.sh krffs/manifests/rach2-recovery.manifest)
MANIFEST_DEPS_rach2_metal_disk := $(shell sh krffs/manifest_deps.sh krffs/manifests/rach2-metal-disk.manifest)
MANIFEST_DEPS_grub_test := $(shell sh krffs/manifest_deps.sh krffs/manifests/grub-test.manifest)
STAGED_IN_MANIFESTS = $(sort $(filter vendor/musl/install/% build/vendor/tcc/%, \
    $(MANIFEST_DEPS_test) $(MANIFEST_DEPS_os_simple) $(MANIFEST_DEPS_os_real) $(MANIFEST_DEPS_rach2_metal) $(MANIFEST_DEPS_rach2_recovery) $(MANIFEST_DEPS_rach2_metal_disk) $(MANIFEST_DEPS_grub_test)))
MUSL_KNOWN = $(sort $(MUSL_OUTPUTS) $(filter vendor/musl/install/%,$(STAGED_IN_MANIFESTS)))
TCC_KNOWN = $(sort $(TCC_OUTPUTS) $(filter build/vendor/tcc/%,$(STAGED_IN_MANIFESTS)))
ifneq ($(MUSL_KNOWN),)
$(MUSL_KNOWN): $(LIBC_STAMP)
endif
ifneq ($(TCC_KNOWN),)
$(TCC_KNOWN): $(TCC_STAMP)
endif
$(BUSYBOX_OUTPUTS): $(BUSYBOX_STAMP)
$(DROPBEAR_BIN): $(DROPBEAR_STAMP)
$(PDPMAKE_BIN): $(PDPMAKE_STAMP)
$(E2FSCK_BIN) $(RESIZE2FS_BIN): $(E2FSPROGS_STAMP)

# --- component files the root graph names. Each gets an explicit delegation
#     rule to its sub-make: without one, a deleted kernel beside its surviving
#     kernel.o would match make's builtin link rule and be relinked by the
#     host cc. The rules run only when the file is missing; the ordinary
#     refresh still happens through the order-only component recursion. ---
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

build/rach2/kernel: ask-the-sub-make
	@$(MAKE) --no-print-directory -C rach2
build/ish/ish: ask-the-sub-make
	@$(MAKE) --no-print-directory -C ish
# One of the three asks the sub-make and the other two wait for it. A rule with
# several targets is a rule per target to make, so the three missing files of a
# clean build were three sub-makes of one directory at once.
build/krffs/mkfs.krffs: ask-the-sub-make | check-libc
	@$(MAKE) --no-print-directory -C krffs
build/krffs/stageimage.krffs build/krffs/fsck.krffs: build/krffs/mkfs.krffs
	@:

# The binary byte searcher (tools/inspect/binfind.c), replacing strings(1) piped
# to grep in the TinyCC policy check.
$(BINFIND): tools/inspect/binfind.c
	@mkdir -p $(dir $@)
	cc -O2 -Wall -Wextra -o $@ $<

# The ELF validator (tools/inspect/elfcheck.c): user executables against the
# loader and the static-only policy, kernels against the multiboot loaders.
$(ELFCHECK): tools/inspect/elfcheck.c
	@mkdir -p $(dir $@)
	cc -O2 -Wall -Wextra -o $@ $<

# The set-id guard (tools/inspect/setidcheck.c), reading its declarations from
# tools/inspect/setid.allow.
$(SETIDCHECK): tools/inspect/setidcheck.c
	@mkdir -p $(dir $@)
	cc -O2 -Wall -Wextra -o $@ $<

# The configuration checkers (tools/inspect/cfgcheck.c): the GRUB core's
# embedded configuration and the sealed disk's fixed address.
$(CFGCHECK): tools/inspect/cfgcheck.c
	@mkdir -p $(dir $@)
	cc -O2 -Wall -Wextra -o $@ $<

# The prose checks (tools/verify/prosecheck.c): comment width, the banned
# vocabulary, the phrase counts the report tabulates, and the dev-log index.
$(PROSECHECK): tools/verify/prosecheck.c
	@mkdir -p $(dir $@)
	cc -O2 -Wall -Wextra -o $@ $<

# The guest-build guard (tools/verify/guestcheck.c): the three guest makefiles
# against the host build they mirror.
$(GUESTCHECK): tools/verify/guestcheck.c
	@mkdir -p $(dir $@)
	cc -O2 -Wall -Wextra -o $@ $<

# The digest tool (tools/verify/sha256.c), replacing sha256sum and shasum. The
# scripts that need it before make has run compile it themselves.
$(SHA256TOOL): tools/verify/sha256.c
	@mkdir -p $(dir $@)
	cc -O2 -Wall -Wextra -o $@ $<

# --- validation stamps. A validator is a prerequisite of a stamp that sits
#     between a published artifact and its consumers, so editing the checker
#     revalidates the artifact without recompiling it. Validation of a fresh
#     candidate happens inside the producing recipe (the kernel link, mkimage)
#     and preserves the previous artifact on failure; a failed REvalidation
#     here removes the published artifact and its stamp and fails, so the
#     next run rebuilds it through the ordinary rules. fsck would clear the
#     image's dirty flag in place, so the image stamp runs only the
#     non-mutating set-id inventory. ---
build/rach2/.kernel-checked: build/rach2/kernel $(ELFCHECK)
	@$(ELFCHECK) --kernel build/rach2/kernel || { rm -f build/rach2/kernel $@; exit 1; }
	@touch $@

vendor/tcc/.validated: $(TCC_STAMP) tools/inspect/check_link.sh
	@sh tools/inspect/check_link.sh vendor/musl/install/lib/libc.a build/vendor/tcc/libtcc1.a \
	    vendor/musl/install/lib/crt1.o vendor/musl/install/lib/crti.o \
	    vendor/musl/install/lib/crtn.o || { rm -rf build/vendor/tcc $@; exit 1; }
	@touch $@

vendor/pdpmake/.validated: $(PDPMAKE_STAMP) $(ELFCHECK)
	@$(ELFCHECK) $(PDPMAKE_BIN) || { rm -f $(PDPMAKE_BIN) $@; exit 1; }
	@touch $@

build/images/%.checked: build/images/%.krffs $(SETIDCHECK) tools/inspect/setid.allow
	@$(SETIDCHECK) --image $< --manifest krffs/manifests/$*.manifest \
	    || { rm -f $< $@; exit 1; }
	@touch $@

all images: build/rach2/.kernel-checked vendor/tcc/.validated vendor/pdpmake/.validated
test-image: build/images/test.checked
os-simple-image: build/images/os-simple.checked
os-real-image: build/images/os-real.checked

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

rach2: build/rach2/kernel
ish: build/ish/ish
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
#     images that carry it. The component sub-makes are order-only: they
#     refresh their outputs first, and the recorded inputs decide staleness. ---

images: test-image os-simple-image os-real-image os-real-ext2

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
# rebuilt hasher must not remint hashes the images already carry.
krffs/shadow: | build/krffs/mkfs.krffs
	sh krffs/gen_demo_password.sh

# `make recovery-key`: put this tree's own key into the recovery system, so a
# machine built here can be updated over the network. An image built without
# that key starts no SSH server at all and the only way in is the keyboard,
# which is what its absence means. `tests/run` mints it before it builds
# anything, since the suites that update a machine do it through recovery over
# SSH; a plain `make` does not, so an image built by hand answers nobody unless
# somebody asked for a key.
.PHONY: recovery-key
recovery-key: krffs/recovery_authorized_keys
	@echo "recovery-key: the recovery system will answer this tree's key"

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
	      $(IMAGES_DIR)/rach2-recovery.krffs $(IMAGES_DIR)/rach2-recovery.iso \
	      $(IMAGES_DIR)/rach2-install-stick.img \
	      $(IMAGES_DIR)/install-test-stick.img $(SEALED_DISK)
	@echo "no-recovery-key: the recovery system will answer nobody"

DEMO_CREDS = krffs/authorized_keys krffs/shadow

IMAGE_TOOLS = krffs/mkimage.sh $(SETIDCHECK) tools/inspect/setid.allow \
              build/krffs/mkfs.krffs build/krffs/stageimage.krffs

-include $(TEST_IMAGE).d $(OS_SIMPLE_IMAGE).d $(OS_REAL_IMAGE).d

# The musl integration programs (tests/musl/*.c -> build/tests/musl/), built by
# their own script. They are end-to-end libc tests staged into the test image.
musl-programs: $(LIBC_STAMP)
	sh tests/musl/build.sh

# The test image: the freestanding syscall probes plus the musl integration
# programs. `make test` boots it and asserts every probe's success token.
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
# dropbear SSH server and client, Doom, and an /etc + /root tree). 16 MB: enough
# for the capstones and the shareware Doom WAD, and it still fits under the
# kernel's 32 MB physical-RAM cap once the kernel and its frames are accounted
# for (the module is loaded just above 1 MB). gen_demo_key.sh provisions the SSH
# login keys and gen_demo_password.sh the account passwords and /etc/shadow,
# each when absent; the check targets ensure the busybox/dropbear binaries are
# real.
$(OS_REAL_IMAGE): $(DEMO_CREDS) krffs/manifests/os-real.manifest $(MANIFEST_DEPS_os_real) $(IMAGE_TOOLS) | preflight krffs rach2 utils check-busybox check-tcc check-pdpmake check-setid check-dropbearmulti ports/doom
	$(MKIMAGE) krffs/manifests/os-real.manifest $@ 16

# The same manifest as an ext2 volume, the sealed disk's fourth partition and
# the root `rach2.root=ext2` mounts: 1 GiB, the partition's size, sparse on
# the host. tools/image/mkext2.sh stages it through stageimage's tree mode,
# formats it with the host's mke2fs on the one profile the driver writes,
# sets the owners with debugfs, and passes it through e2fsck and the set-id
# guard's own ext2 reader before it is named. The prerequisites are the KRFFS
# image's, so the two are built from one tree.
$(OS_REAL_EXT2): $(DEMO_CREDS) krffs/manifests/os-real.manifest $(MANIFEST_DEPS_os_real) tools/image/mkext2.sh $(IMAGE_TOOLS) | preflight krffs rach2 utils check-busybox check-tcc check-pdpmake check-setid check-dropbearmulti ports/doom
	sh tools/image/mkext2.sh krffs/manifests/os-real.manifest $@ 1024

# The metal image: what the Lenovo G50-70 boots from a USB stick. A RAM-only
# machine (the kernel has no USB driver, so the module GRUB loads is the whole
# system), so it carries a shell, dropbear, the account files and the keys, and
# leaves out the compiler, /src, Doom and bootctl, which a machine with no
# writable disk cannot use. 4 MB: the binaries come to about 1.4 MB, and the
# rest is headroom under the kernel's 32 MB direct map. The metal kernel: the
# same sources built read-only at the disk, into its own object tree so the two
# kernels never share one. The switch is a build switch and not a boot flag
# because a boot flag is editable at the GRUB prompt, and what it protects is
# somebody else's Windows installation. check_diskpolicy.sh then reads the
# policy back off the artifact, since there is nothing to check for by absence:
# the commands are numbers and the functions that issue them are static.
METAL_KERNEL = build/rach2-metal/kernel
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
RECOVERY_KERNEL = build/rach2-recovery/kernel
$(RECOVERY_KERNEL): ask-the-sub-make
	@$(MAKE) --no-print-directory -C rach2 COMPONENT=rach2-recovery KEXTRA=-DRACH2_RECOVERY
# The file rule above is the only place this kernel is built. Running the
# sub-make here as well gave two invocations with no edge between them, and a
# parallel build could reach both: they write the same object files, so one
# could link while the other rewrote an object under it.
rach2-recovery: krffs rach2 $(RECOVERY_KERNEL)
	sh tools/inspect/check_recovery.sh $(RECOVERY_KERNEL) recovery
	sh tools/inspect/check_recovery.sh build/rach2/kernel ordinary
	sh tools/inspect/check_diskpolicy.sh $(RECOVERY_KERNEL) read-write

# A real file target with the components as order-only prerequisites, the shape
# $(TEST_IMAGE) above already uses. As a phony target this rebuilt the image on
# every invocation, so three suites that merely boot it each restaged it, and
# two of them running at once restaged it under each other.
$(METAL_IMAGE): $(DEMO_CREDS) krffs/manifests/rach2-metal.manifest $(MANIFEST_DEPS_rach2_metal) $(IMAGE_TOOLS) | preflight krffs rach2 rach2-metal utils check-busybox check-setid check-dropbearmulti
	$(MKIMAGE) krffs/manifests/rach2-metal.manifest $@ 4

rach2-metal-image: $(METAL_IMAGE)

# The recovery RAM image: what the recovery kernel loads as its root. It is the
# metal profile plus the two programs that repair a disk, and it mounts nothing
# from the disk it is repairing, which is what lets it rewrite that disk's root.
#
# A recovery image that carries a key and one that carries none are identical
# from outside a booted machine, so the difference is in the name of every
# artifact that carries it: with a key the image is `rach2-recovery-with-key`,
# and so are the ISO and the install stick built from it. A person about to
# write a stick reads the name of the file being written.
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
# partition is larger than the 32 MB resident page-cache cap on purpose, so it
# can later prove an on-disk file system bigger than what fits in RAM at once.
os-real-disk: krffs rach2 utils check-busybox check-tcc check-pdpmake check-setid check-dropbearmulti ports/doom
	$(MKDISK) krffs/manifests/os-real.manifest $(OS_REAL_DISK) 64 48

# `make rach2-sealed-disk`: the disk milestone 25 seals into the laptop, written
# once over a USB adapter and not removed again. Its geometry is the milestone's
# contract and every number here is checked against the built image: a 64 MiB
# FAT32 boot partition at 4 KiB clusters (one sector per cluster puts a 64 MiB
# volume past both of bootslot's bounds), a 500 MiB root, and a 64 MiB recovery
# partition whose kernel and RAM image GRUB can reach when partition 1 cannot be
# read at all. The fourth partition is the ext2 root, 1 GiB at 630 MiB, where
# the installed machine's fourth partition already begins, the same manifest
# as an ext2 volume, which the candidate entries boot while the fallback
# entries keep the KRFFS root, so a candidate root that does not come up falls
# back through the trial to a system that does. The image is 1655 MiB,
# 3389440 sectors.
#
# The address is `rach2.ip=none`: this machine is carried between buildings and
# takes whatever the network there gives it, through `ifup -a` and udhcpc, which
# init runs before it starts the SSH server. A fixed address on the boot line
# would be one more thing to edit at the desk, and editing it is the thing this
# milestone exists to avoid. The two static-address entries in the menu are the
# answer to a network with no server at all, and they carry a fixed address
# instead: a line cannot carry both, since the kernel refuses an address given
# twice and the machine would come up with none.
#
# This target is the geometry and the boot line. The boot menu, the GRUB core
# slots and the recovery contents are added by the phases that own them. The
# fixed address the two static entries carry, for the day the network this
# machine is plugged into has no server: the same defaults the metal stick uses,
# overridable at build time for anywhere else. The menu takes a prefix and the
# recovery script takes a dotted mask, and the two must be the same subnet with
# the gateway inside it; the configuration checker holds them to that before a
# disk is written, since a machine whose menu and fallback disagree is one
# nobody can reach by either.
SEALED_ADDRESS ?= 192.168.0.105
SEALED_MASK ?= 255.255.255.0
SEALED_PREFIX ?= 24
SEALED_GATEWAY ?= 192.168.0.1
SEALED_IP = $(SEALED_ADDRESS)/$(SEALED_PREFIX)
SEALED_DISK = build/images/rach2-sealed.disk

# The file systems in files a fresh disk carries in /demo, one of each kind
# this kernel mounts, each with a hello.txt in it, and an empty KRFFS volume
# to copy when a person wants an image of their own. They are built here
# rather than staged from somewhere, because what they are is what the host's
# own formatters write.
FS_DEMO_DIR = build/images/fs-demo
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
SEALED_EXT2 = build/images/rach2-metal-disk.ext2
$(SEALED_EXT2): $(FS_DEMO_IMAGES) $(DEMO_CREDS) krffs/manifests/rach2-metal-disk.manifest $(MANIFEST_DEPS_rach2_metal_disk) tools/image/mkext2.sh $(IMAGE_TOOLS) | preflight krffs rach2 utils check-busybox check-tcc check-pdpmake check-setid check-dropbearmulti ports/doom
	sh tools/image/mkext2.sh krffs/manifests/rach2-metal-disk.manifest $@ 1024
# The alias builds everything the disk is made of, then asks for the disk
# itself. The two are separate because the components are phony targets, which
# are always out of date, and a disk that named them directly was rebuilt on
# every invocation: half a minute, on a machine that had not changed.
rach2-sealed-disk: krffs rach2 utils rach2-recovery rach2-recovery-image check-busybox check-tcc check-pdpmake check-setid check-dropbearmulti ports/doom $(CFGCHECK)
	@$(MAKE) --no-print-directory $(SEALED_DISK)

$(SEALED_DISK): build/rach2/kernel $(RECOVERY_KERNEL) $(RECOVERY_IMAGE) $(SEALED_EXT2) \
                krffs/manifests/rach2-metal-disk.manifest krffs/rescue-static.script \
                tools/image/mkdisk_grub.sh $(CFGCHECK)
	@$(CFGCHECK) sealednet $(SEALED_ADDRESS) $(SEALED_PREFIX) \
	    $(SEALED_MASK) $(SEALED_GATEWAY) krffs/rescue-static.script
	RACH2_DISK_BOOT_MB=64 \
	RACH2_DISK_CLUSTER=8 \
	RACH2_DISK_RECOVERY_MB=64 \
	RACH2_DISK_APPEND="rach2.disk=ahci rach2.netconsole" \
	RACH2_DISK_DHCP_APPEND="rach2.ip=none" \
	RACH2_DISK_STATIC_APPEND="rach2.ip=$(SEALED_IP) rach2.gateway=$(SEALED_GATEWAY)" \
	RACH2_DISK_RECOVERY_KERNEL=$(RECOVERY_KERNEL) \
	RACH2_DISK_RECOVERY_IMAGE=$(RECOVERY_IMAGE) \
	RACH2_DISK_EXT2_MB=1024 \
	RACH2_DISK_EXT2_IMAGE=$(SEALED_EXT2) \
	RACH2_DISK_HELLO=krffs/demo/hello.txt \
	    sh tools/image/mkdisk_grub.sh build/rach2/kernel $(SEALED_DISK)


# The demo credentials, as file targets. Both generators are idempotent and do
# nothing when their output is there, which was enough while one thing ran at a
# time: with `make -j` and a pool of suites, two of them mint a key at once and
# race on the temporary file, which is `mv: authorized_keys.tmp: No such file
# or directory`. Make builds each once, and everything that needs them says so.
krffs/authorized_keys:

krffs/shadow:

DEMO_CREDS = krffs/authorized_keys krffs/shadow


# The public test entry: the source checks, the native tests, and the core
# tier, in that order. The first two rebuild the kernel with their own flags
# and share a scratch directory, so they run alone and before the pool; the
# tier is the system suites that between them exercise every subsystem, chosen
# to finish in under two minutes. The runner owns the registry, so this holds
# no suite knowledge of its own.
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
	sh tools/image/mkiso.sh test
os-simple-iso: os-simple-image
	sh tools/image/mkiso.sh os-simple
os-real-iso: os-real-image
	sh tools/image/mkiso.sh os-real
rach2-metal-iso: rach2-metal rach2-metal-image
	sh tools/image/mkiso.sh rach2-metal
rach2-recovery-iso: rach2-recovery rach2-recovery-image
	sh tools/image/mkiso.sh $(RECOVERY_NAME)

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
# executable" and this kernel would refuse it, and a dynamically linked one it
# would ACCEPT and then run unrelocated, which is worse. The last check reads
# the output policy's own diagnostics out of the shipped binary, since a patch
# that did not apply and a compiler that accepts `-run` look identical from
# outside.
check-pdpmake: $(PDPMAKE_STAMP) $(PDPMAKE_BIN) $(ELFCHECK)
	@test -s $(PDPMAKE_BIN) || { \
	    echo "pdpmake: $(PDPMAKE_BIN) missing or empty; rm vendor/pdpmake/.stamp and rebuild (make pdpmake)" >&2; \
	    exit 1; }
	@$(ELFCHECK) $(PDPMAKE_BIN)

# The port's size limits, each set from its first build with a tenth of
# headroom, e2fsck's in milestone 42 at 673672 bytes stripped and
# resize2fs's in milestone 47: a configure that let a library or a program
# back in would be caught here and not in the recovery image.
E2FSCK_LIMIT = 737280
RESIZE2FS_LIMIT = 430080
check-e2fsck: $(E2FSPROGS_STAMP) $(E2FSCK_BIN) $(RESIZE2FS_BIN) $(ELFCHECK)
	@for f in $(E2FSCK_BIN) $(RESIZE2FS_BIN); do test -s $$f || { \
	    echo "e2fsprogs: $$f missing or empty; rm vendor/e2fsprogs/.stamp and rebuild (make e2fsprogs)" >&2; \
	    exit 1; }; done
	@$(ELFCHECK) $(E2FSCK_BIN)
	@$(ELFCHECK) $(RESIZE2FS_BIN)
	@size=$$(wc -c < $(E2FSCK_BIN) | tr -d ' '); test "$$size" -le $(E2FSCK_LIMIT) || { \
	    echo "e2fsprogs: e2fsck is $$size bytes, over the $(E2FSCK_LIMIT) limit vendor/e2fsprogs/Readme.md sets" >&2; \
	    exit 1; }
	@size=$$(wc -c < $(RESIZE2FS_BIN) | tr -d ' '); test "$$size" -le $(RESIZE2FS_LIMIT) || { \
	    echo "e2fsprogs: resize2fs is $$size bytes, over the $(RESIZE2FS_LIMIT) limit vendor/e2fsprogs/Readme.md sets" >&2; \
	    exit 1; }

check-tcc: $(TCC_STAMP) $(TCC_OUTPUTS) $(BINFIND) $(ELFCHECK)
	@for f in $(TCC_BIN) build/vendor/tcc/libtcc1.a \
	          build/vendor/tcc/include/stdarg.h \
	          build/vendor/tcc/include/stddef.h \
	          build/vendor/tcc/include/stdbool.h \
	          build/vendor/tcc/include/float.h \
	          build/vendor/tcc/include/varargs.h; do \
	    test -s "$$f" || { echo "tcc: $$f missing or empty; rm vendor/tcc/.stamp and rebuild (make tcc)" >&2; exit 1; }; \
	done
	@$(ELFCHECK) --selftest
	@$(ELFCHECK) $(TCC_BIN)
	@for m in "must be linked with -static" "run is not available" \
	          "shared is not available" "must be an ELF"; do \
	    $(BINFIND) "$$m" $(TCC_BIN) || { echo "tcc: the shipped compiler does not carry the output policy ($$m); rm vendor/tcc/.stamp and rebuild" >&2; exit 1; }; \
	done

check-busybox: $(BUSYBOX_STAMP) $(BUSYBOX_OUTPUTS)
	@for f in $(BUSYBOX_BIN) $(BUSYBOX_SUID_BIN) \
	          build/vendor/busybox/applet_tables-main.h \
	          build/vendor/busybox/applet_tables-suid.h \
	          build/vendor/busybox/config-main \
	          build/vendor/busybox/config-suid; do \
	    test -s "$$f" || { echo "busybox: $$f missing or empty; rm vendor/busybox/.stamp and rebuild (make busybox)" >&2; exit 1; }; \
	done

# Privilege climbs through a declared set of paths and no others. The sweep
# reads every manifest against a per-image allowlist, so a set-id line anywhere,
# under any of the five mode-bearing directives, fails the build rather than a
# boot. Every artifact a check reads becomes a prerequisite here, in the phase
# that adds the check, because make does not order the siblings of a
# prerequisite list. The manifest sweep needs no build product, but the
# applet-table check reads one, so this target is ordered against the busybox
# stamp. Unordered it would fail a correct tree on a header that does not exist
# yet, or, in the case that matters, read the PREVIOUS configuration's table
# after rach2.config was edited, pass, and let the image stage the busybox whose
# re-enabled applet the check exists to forbid: build_busybox.sh opens by
# removing and re-extracting the tree, so for most of a rebuild the header is
# absent and before that it is stale. The applet half reads a generated header
# and the init half reads two built binaries, so both are ordered after what
# produces them, the way phase 3's rule requires: unordered, a parallel build
# scans a build/utils/sbin/ that holds yesterday's artifacts and passes while
# today's are staged.
check-setid: $(BUSYBOX_STAMP) utils $(SETIDCHECK)
	@$(SETIDCHECK)
	@$(SETIDCHECK) --applets
check-dropbearmulti: $(DROPBEAR_STAMP) $(DROPBEAR_BIN)
	@test -s $(DROPBEAR_BIN) || { echo "dropbear: $(DROPBEAR_BIN) missing or empty; rm vendor/dropbear/.stamp and rebuild (make dropbear)" >&2; exit 1; }

# musl's stamp guards two artifacts, not one: the installed libc the cross links
# against, and the EXTRACTED source tree, which krffs (mkhash's crypt) and the
# mkhash host test compile from directly. A copied worktree carries the stamp
# without either, so check both here rather than let a consumer fail later on a
# missing file with no cause named.
check-libc: $(LIBC_STAMP) $(MUSL_OUTPUTS)
	@test -s $(LIBC_A) || { echo "libc: $(LIBC_A) missing or empty; rm vendor/musl/.stamp and rebuild (make)" >&2; exit 1; }
	@test -f $(MUSL_CRYPT_SRC) || { echo "libc: $(MUSL_CRYPT_SRC) missing; rm vendor/musl/.stamp and rebuild (make)" >&2; exit 1; }

# The test suites: every `run-NAME` target, the fixtures they write, the eight
# test tools, and `prepare-all`. Included rather than kept apart so that one
# dependency graph covers the product and the variants only a suite needs.
include tests/system/suites.mk

# Fail if a rach2 .c on disk is missing from C_SRC, so a new kernel source
# cannot be dropped from the build without anybody noticing. `tests/run style`
# runs this.
check-sources:
	@./tools/verify/check_sources.sh

# clean: the everyday reset. Each sub-make removes its own build/<component>
# tree (objects, .d, binaries), and the RAM-disk images under build/images go
# too. The expensive vendored third-party is LEFT in place: vendor/musl/install,
# the extracted busybox/dropbear trees, and the staged capstones under
# build/vendor (which no sub-make owns). Use distclean for those.
clean:
	@for dir in $(BASE_DIRS) $(FULL_DIRS); do $(MAKE) -C $$dir clean; done
	rm -rf build/images build/iso

# distclean: a full from-scratch reset; clean plus every third-party build
# product: the stamps, vendor/musl/install, the extracted vendored source trees,
# and the entire out-of-tree build/ (including the staged capstones under
# build/vendor that a routine clean leaves in place). The next build rebuilds
# musl/busybox/dropbear/tcc/pdpmake from the tarballs. A stamp left behind while
# its binary went with build/ is the failure this list exists to prevent: the
# next build would skip the work and fail at the check instead.
distclean: clean
	rm -f $(LIBC_STAMP) $(BUSYBOX_STAMP) $(DROPBEAR_STAMP) $(TCC_STAMP) \
	      $(PDPMAKE_STAMP) $(E2FSPROGS_STAMP) vendor/musl/.outputs vendor/tcc/.outputs \
	      vendor/tcc/.validated vendor/pdpmake/.validated
	rm -rf vendor/musl/install \
	       $(LIBC_TARBALL:.tar.gz=) $(BUSYBOX_TARBALL:.tar.bz2=) \
	       $(DROPBEAR_TARBALL:.tar.bz2=) $(TCC_TARBALL:.tar.bz2=) \
	       $(PDPMAKE_TARBALL:.tgz=) $(E2FSPROGS_TARBALL:.tar.gz=)
	rm -rf build


