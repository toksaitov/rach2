# Rach2 root Makefile.
#
# The build produces purpose-built RAM-disk images, each its own file under
# build/images/ so they coexist: `test` (the automated end-to-end test binaries),
# `os-simple` (a small Unix built out of this project's own code: our login,
# shell and Unix tools), and `os-real` (the same jobs taken from the code a real
# system ships: busybox, dropbear, Doom, and the tcc compiler with pdpmake to
# drive it).
#
# Quick start (the build checks its own tools; sh tools/setup/doctor.sh is the wider report):
#   make            build everything: kernel, host tools, guest programs, the
#                   vendored userland, Doom, and the three RAM images. The
#                   first build compiles the five vendor projects and takes
#                   minutes; later runs rebuild only what changed.
#   make test       boot the test image and check every probe passes.
#   tests/run NAME  run one named suite (style, host, ssh, all, ...).
#   make isos       a bootable ISO per image, for real hardware (needs Docker).
#   make clean      remove the in-house build products and the images
#   make distclean  + the vendored musl, busybox, dropbear, tcc and pdpmake
#                   builds (full from-scratch)

# Run a built image with rach2/run.sh <test|os-simple|os-real>; the build system
# does not run QEMU (that script models the disk/audio/network/-cpu matrix).

# --- components (each builds via its own sub-make) ---
BASE_DIRS = krffs rach2 tests ish utils
FULL_DIRS = ports/doom

# --- vendored third-party: built by their own scripts (which wrap musl's autotools,
#     busybox/dropbear's kbuild, tcc's hand-written configure and pdpmake's bare
#     POSIX makefile), tracked with stamp files so a clean clone
#     "just works" without running the build_*.sh by hand, and each rebuilds only
#     when its real inputs (tarball, build script, config) change. ---
LIBC_STAMP     = vendor/musl/.stamp
BUSYBOX_STAMP  = vendor/busybox/.stamp
DROPBEAR_STAMP = vendor/dropbear/.stamp
TCC_STAMP      = vendor/tcc/.stamp
PDPMAKE_STAMP  = vendor/pdpmake/.stamp

# The staged capstone binaries the build_*.sh scripts produce, out of tree under
# build/vendor/ (mirroring their vendor/<name>/ sources). A routine `make clean` never
# reaches build/vendor, so these expensive artifacts survive it; only `make distclean`
# removes them. They are the REAL prerequisites of the capstone image (see the check
# targets below): a stamp only proves its build RAN.
BUSYBOX_BIN  = build/vendor/busybox/busybox
BUSYBOX_SUID_BIN = build/vendor/busybox/busybox-suid
DROPBEAR_BIN = build/vendor/dropbear/dropbearmulti
TCC_BIN      = build/vendor/tcc/tcc
PDPMAKE_BIN  = build/vendor/pdpmake/make

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

# The two musl artifacts consumers depend on, both derived from the tarball name so
# the pinned version is written once: the installed static libc the cross-compiled
# userland links against, and the extracted source, which the host-compiled mkhash
# and its host test read the SHA-512 crypt from.
LIBC_A         = vendor/musl/install/lib/libc.a
MUSL_CRYPT_SRC = $(LIBC_TARBALL:.tar.gz=)/src/crypt/crypt_sha512.c

# --- RAM-disk images. Each is built out of tree from a checked-in manifest by the
#     manifest-driven stageimage.krffs (see krffs/mkimage.sh and krffs/manifests/),
#     which writes the whole tree directly as KRFFS v2 nodes (no FUSE mount). Each
#     image is its own file, so they coexist. ---
IMAGES_DIR      = build/images
TEST_IMAGE      = $(IMAGES_DIR)/test.krffs
OS_SIMPLE_IMAGE = $(IMAGES_DIR)/os-simple.krffs
OS_REAL_IMAGE   = $(IMAGES_DIR)/os-real.krffs
METAL_IMAGE     = $(IMAGES_DIR)/rach2-metal.krffs
OS_REAL_DISK    = $(IMAGES_DIR)/os-real.disk
MKIMAGE         = krffs/mkimage.sh
# The suites' wall-clock limit tool (see its rule below).
DEADLINE        = build/tests/system/deadline
BINFIND         = build/tools/inspect/binfind
MKDISK          = krffs/mkdisk.sh

.PHONY: all full deps clean distclean images musl-programs test test-disk bootpart-test install-test \
        test-noentropy \
        test-image os-simple-image os-real-image os-real-disk persist-test \
        cred-persist-test \
        ping-test packet-test raw-test bootline-test netif-test dgram-test dhcp-test netconsole-test test-addressed metal-test rach2-metal diskpolicy-test \
        subset-test diskdiag-test reset-test \
        ping-test account-test shutdown-test login-test exitline-test procfiles-test crash-test stress-test os-real-grub-disk \
        tcc-test ssh-test make-test selfbuild-test \
        test-disk-ahci test-disk-q35 persist-test-ahci stress-test-ahci rach2-metal-grub-disk \
        rach2-sealed-disk rach2-recovery rach2-recovery-image authority-test menu-test grub-test \
        drill-test sealed-test \
        isos test-iso os-simple-iso os-real-iso rach2-metal-image rach2-metal-iso \
        check-busybox check-setid check-dropbearmulti check-libc check-tcc \
        check-pdpmake \
        compile-commands check-sources \
        verify-style verify-host run-style run-host run-default preflight doctor help \
        musl busybox dropbear tcc pdpmake $(BASE_DIRS) $(FULL_DIRS)

# Default: everything reusable. The three RAM images pull in the kernel, the
# host tools, the guest programs, the vendored userland, and Doom through
# their own prerequisites. The first build compiles five vendor projects and
# takes minutes; a later `make` rebuilds only what changed. Each image is its
# own file under build/images/, so they coexist instead of overwriting one
# shared image.
all: images $(DEADLINE)

# The cached tool check (tools/setup/preflight.sh): a missing or changed build tool
# fails here, named, before any compilation. It is an order-only prerequisite
# of the build entry points; clean and distclean never run it.
preflight:
	@./tools/setup/preflight.sh

all images $(BASE_DIRS) $(FULL_DIRS): | preflight
$(LIBC_STAMP) $(BUSYBOX_STAMP) $(DROPBEAR_STAMP) $(TCC_STAMP) $(PDPMAKE_STAMP): | preflight

# Retired names, kept as aliases for this milestone only. `full` was the old
# everything build; `deps` built vendor stamps that are now ordinary
# prerequisites of the images.
full:
	@echo "full: retired: plain make builds everything now" >&2
	@$(MAKE) --no-print-directory all

deps:
	@echo "deps: retired: vendor builds are ordinary prerequisites of make" >&2
	@$(MAKE) --no-print-directory $(LIBC_STAMP) $(BUSYBOX_STAMP) $(DROPBEAR_STAMP) \
	    $(TCC_STAMP) $(PDPMAKE_STAMP)

# Friendly aliases for the third-party stamps.
musl: $(LIBC_STAMP)
busybox: $(BUSYBOX_STAMP)
dropbear: $(DROPBEAR_STAMP)
tcc: $(TCC_STAMP)
pdpmake: $(PDPMAKE_STAMP)

# --- third-party stamp rules. Each removes BOTH the stamp and the expected artifact
#     up front, runs its build script, requires a non-empty artifact, then touches
#     the stamp. Removing the artifact first means a script that exits 0 without
#     restaging cannot pass the assertion on a STALE artifact; removing the stamp
#     first means any failure (build or assertion) leaves no stamp, so the next make
#     retries instead of trusting a missing/old artifact. .DELETE_ON_ERROR is the
#     belt-and-suspenders backstop on the final touch. ---
.DELETE_ON_ERROR:

# The patch manifest and the pinned patches are prerequisites, not
# decoration: build_libc.sh applies the patches patches/series names after
# the extract, so a tree whose stamp predates a series edit would keep
# linking every program against the wrong libc, and an edit to one patch
# would never be built at all. The series file notices an added or removed
# patch; the wildcard notices an edit to one that exists.
LIBC_PATCHES = vendor/musl/patches/series $(wildcard vendor/musl/patches/*.patch)

$(LIBC_STAMP): vendor/musl/build_libc.sh $(LIBC_TARBALL) $(LIBC_PATCHES)
	@rm -f $@ vendor/musl/install/lib/libc.a
	+@t0=$$(date +%s); ./vendor/musl/build_libc.sh && echo "libc: result: built in $$(($$(date +%s)-t0))s"
	@test -s vendor/musl/install/lib/libc.a || { echo "libc: build produced no libc.a" >&2; exit 1; }
	@{ find vendor/musl/install -type f | sort; echo $(MUSL_CRYPT_SRC); } > vendor/musl/.outputs
	@touch $@

# The patches are a prerequisite for the same reason musl's are: `build_busybox.sh`
# applies everything under vendor/busybox/patches/ after it extracts, so a stamp
# that did not depend on them would keep shipping a binary built from the patch
# before the edit.
# The directory is a prerequisite beside the wildcard: the wildcard notices an
# edit to a patch that exists, and the directory's own timestamp notices one
# being added or taken away, including the last one.
BUSYBOX_PATCHES = vendor/busybox/patches $(wildcard vendor/busybox/patches/*.patch)

$(BUSYBOX_STAMP): vendor/busybox/build_busybox.sh vendor/busybox/rach2.config \
                  vendor/busybox/rach2-suid.config vendor/busybox/muslgcc \
                  $(BUSYBOX_TARBALL) $(BUSYBOX_PATCHES) $(LIBC_STAMP)
	@rm -f $@ $(BUSYBOX_BIN) $(BUSYBOX_SUID_BIN) \
	    build/vendor/busybox/applet_tables-main.h \
	    build/vendor/busybox/applet_tables-suid.h \
	    build/vendor/busybox/config-main \
	    build/vendor/busybox/config-suid
	+@t0=$$(date +%s); ./vendor/busybox/build_busybox.sh && echo "busybox: result: built in $$(($$(date +%s)-t0))s"
	@test -s $(BUSYBOX_BIN) || { echo "busybox: build produced no $(BUSYBOX_BIN)" >&2; exit 1; }
	@touch $@

$(TCC_STAMP): vendor/tcc/build_tcc.sh vendor/tcc/rach2-output-policy.patch \
              vendor/busybox/muslgcc $(TCC_TARBALL) $(LIBC_STAMP)
	@rm -f $@ $(TCC_BIN) build/vendor/tcc/libtcc1.a \
	    build/vendor/tcc/include/stdarg.h
	+@t0=$$(date +%s); ./vendor/tcc/build_tcc.sh && echo "tcc: result: built in $$(($$(date +%s)-t0))s"
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
	+@t0=$$(date +%s); ./vendor/pdpmake/build_pdpmake.sh && echo "pdpmake: result: built in $$(($$(date +%s)-t0))s"
	@test -s $(PDPMAKE_BIN) || { echo "pdpmake: build produced no $(PDPMAKE_BIN)" >&2; exit 1; }
	@touch $@

# The patch manifest is a prerequisite for the reason musl's is: the script
# applies what patches/series names, so a series edit or a patch edit must
# invalidate the stamp.
DROPBEAR_PATCHES = vendor/dropbear/patches/series $(wildcard vendor/dropbear/patches/*.patch)

$(DROPBEAR_STAMP): vendor/dropbear/build_dropbear.sh vendor/dropbear/localoptions.h vendor/dropbear/muslgcc \
                   $(DROPBEAR_PATCHES) $(DROPBEAR_TARBALL) $(LIBC_STAMP)
	@rm -f $@ $(DROPBEAR_BIN)
	+@t0=$$(date +%s); ./vendor/dropbear/build_dropbear.sh && echo "dropbear: result: built in $$(($$(date +%s)-t0))s"
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

# The tree lists above test existence only, because musl installs several
# legitimately empty headers. The binaries that can never be empty also get a
# size test, so a truncated one rebuilds its vendor instead of failing a
# check with advice.
_ := $(shell [ ! -f $(LIBC_STAMP) ] || [ -s vendor/musl/install/lib/libc.a ] || rm -f $(LIBC_STAMP))
_ := $(shell [ ! -f $(TCC_STAMP) ] || [ -s $(TCC_BIN) ] || rm -f $(TCC_STAMP))
_ := $(shell [ ! -f $(BUSYBOX_STAMP) ] || { [ -s $(BUSYBOX_BIN) ] && [ -s $(BUSYBOX_SUID_BIN) ]; } || rm -f $(BUSYBOX_STAMP))

ifneq ($(MUSL_OUTPUTS),)
$(MUSL_OUTPUTS): $(LIBC_STAMP)
endif
ifneq ($(TCC_OUTPUTS),)
$(TCC_OUTPUTS): $(TCC_STAMP)
endif
$(BUSYBOX_OUTPUTS): $(BUSYBOX_STAMP)
$(DROPBEAR_BIN): $(DROPBEAR_STAMP)
$(PDPMAKE_BIN): $(PDPMAKE_STAMP)

# --- component files the root graph names. Each gets an explicit delegation
#     rule to its sub-make: without one, a deleted kernel beside its surviving
#     kernel.o would match make's builtin link rule and be relinked by the
#     host cc. The rules run only when the file is missing; the ordinary
#     refresh still happens through the order-only component recursion. ---
build/rach2/kernel:
	@$(MAKE) --no-print-directory -C rach2
build/ish/ish:
	@$(MAKE) --no-print-directory -C ish
build/krffs/mkfs.krffs build/krffs/stageimage.krffs build/krffs/fsck.krffs: | check-libc
	@$(MAKE) --no-print-directory -C krffs

# The deadline tool: the suites' wall-clock limit with process-group kill,
# replacing coreutils' gtimeout. Built with the host compiler like the KRFFS
# tools.
$(DEADLINE): tests/system/runner/deadline.c
	@mkdir -p $(dir $@)
	cc -O2 -Wall -Wextra -o $@ $<

# The binary byte searcher (tools/inspect/binfind.c), replacing strings(1)
# piped to grep in the TinyCC policy check.
$(BINFIND): tools/inspect/binfind.c
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
build/rach2/.kernel-checked: build/rach2/kernel tools/inspect/check_kernel.py
	@python3 tools/inspect/check_kernel.py build/rach2/kernel || { rm -f build/rach2/kernel $@; exit 1; }
	@touch $@

vendor/tcc/.validated: $(TCC_STAMP) tools/inspect/check_link.py
	@python3 tools/inspect/check_link.py vendor/musl/install/lib/libc.a build/vendor/tcc/libtcc1.a \
	    vendor/musl/install/lib/crt1.o vendor/musl/install/lib/crti.o \
	    vendor/musl/install/lib/crtn.o || { rm -rf build/vendor/tcc $@; exit 1; }
	@touch $@

vendor/pdpmake/.validated: $(PDPMAKE_STAMP) tools/inspect/check_elf.py
	@python3 tools/inspect/check_elf.py $(PDPMAKE_BIN) || { rm -f $(PDPMAKE_BIN) $@; exit 1; }
	@touch $@

build/images/%.checked: build/images/%.krffs tools/inspect/check_setid.py
	@python3 tools/inspect/check_setid.py --image $< --manifest krffs/manifests/$*.manifest \
	    || { rm -f $< $@; exit 1; }
	@touch $@

all images: build/rach2/.kernel-checked vendor/tcc/.validated vendor/pdpmake/.validated
test-image: build/images/test.checked
os-simple-image: build/images/os-simple.checked
os-real-image: build/images/os-real.checked

# --- component sub-makes + the build-order DAG ---
$(BASE_DIRS) $(FULL_DIRS):
	@echo "$(notdir $@): building"
	@$(MAKE) --no-print-directory -C $@

# krffs waits on the vendored musl because mkhash compiles its SHA-512 crypt from
# that source, which exists only once build_libc.sh has extracted the pinned
# tarball; without this a from-scratch tree (a fresh clone, or make distclean)
# builds krffs first and cannot find the file.
# Only real edges: krffs compiles the extracted musl crypt source, and tests
# and utils link the installed musl. The kernel, ISH, Doom, and the KRFFS
# tools share no build inputs with one another, so `make -j` overlaps them.
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

images: test-image os-simple-image os-real-image

test-image: $(TEST_IMAGE)
os-simple-image: $(OS_SIMPLE_IMAGE)
os-real-image: $(OS_REAL_IMAGE)

IMAGE_TOOLS = krffs/mkimage.sh tools/inspect/check_setid.py build/krffs/mkfs.krffs \
              build/krffs/stageimage.krffs

-include $(TEST_IMAGE).d $(OS_SIMPLE_IMAGE).d $(OS_REAL_IMAGE).d

# The musl integration programs (tests/musl/*.c -> build/tests/musl/), built by
# their own script. They are end-to-end libc tests staged into the test image.
musl-programs: $(LIBC_STAMP)
	sh tests/musl/build.sh

# The test image: the freestanding syscall probes plus the musl integration
# programs. `make test` boots it and asserts every probe's success token.
$(TEST_IMAGE): krffs/manifests/test.manifest $(IMAGE_TOOLS) | preflight krffs rach2 tests musl-programs utils ish
	$(MKIMAGE) krffs/manifests/test.manifest $@ 10

# The os-simple image: a small Unix built out of this project's own code. init
# execs /bin/login, so this image needs the account databases; gen_demo_password.sh
# mints them when krffs/shadow is absent and does nothing when it is there, and it
# is the same file os-real stages, so one password opens either image.
$(OS_SIMPLE_IMAGE): krffs/manifests/os-simple.manifest $(IMAGE_TOOLS) | preflight krffs rach2 ish utils
	sh krffs/gen_demo_password.sh
	$(MKIMAGE) krffs/manifests/os-simple.manifest $@ 10

# The os-real image: a real Unix-like environment (busybox shell + coreutils,
# dropbear SSH server and client, Doom, and an /etc + /root tree). 16 MB: enough for
# the capstones and the shareware Doom WAD, and it still fits under the kernel's
# 32 MB physical-RAM cap once the kernel and its frames are accounted for (the module
# is loaded just above 1 MB). gen_demo_key.sh provisions the SSH login keys and
# gen_demo_password.sh the account passwords and /etc/shadow, each when absent;
# the check targets ensure the busybox/dropbear binaries are real.
$(OS_REAL_IMAGE): krffs/manifests/os-real.manifest $(IMAGE_TOOLS) | preflight krffs rach2 utils check-busybox check-tcc check-pdpmake check-setid check-dropbearmulti ports/doom
	sh krffs/gen_demo_key.sh
	sh krffs/gen_demo_password.sh
	$(MKIMAGE) krffs/manifests/os-real.manifest $@ 16

# The metal image: what the Lenovo G50-70 boots from a USB stick. A RAM-only
# machine (the kernel has no USB driver, so the module GRUB loads is the whole
# system), so it carries a shell, dropbear, the account files and the keys, and
# leaves out the compiler, /src, Doom and bootctl, which a machine with
# no writable disk cannot use. 4 MB: the binaries come to about 1.4 MB, and the
# rest is headroom under the kernel's 32 MB direct map.
# The metal kernel: the same sources built read-only at the disk, into its own
# object tree so the two kernels never share one. The switch is a build switch
# and not a boot flag because a boot flag is editable at the GRUB prompt, and
# what it protects is somebody else's Windows installation. check_diskpolicy.py
# then reads the policy back off the artifact, since there is nothing to check
# for by absence: the commands are numbers and the functions that issue them
# are static.
METAL_KERNEL = build/rach2-metal/kernel
rach2-metal: krffs
	$(MAKE) -C rach2 COMPONENT=rach2-metal KEXTRA=-DRACH2_DISK_READONLY
	python3 tools/inspect/check_diskpolicy.py $(METAL_KERNEL) read-only

# The recovery kernel: the same sources, built with the authority to write any
# sector of the disk, into its own object tree so the two kernels never share
# one. A build switch and not a boot flag, for the reason above: a flag is
# editable at the GRUB prompt, and what this one guards is the whole disk rather
# than one file. It is NOT the read-only switch and must not be confused with
# it; that one is enforced below devfs and would stop this kernel writing the
# boot partition too, which recovery needs to do. check_recovery.py reads the
# authority back off the artifact in both directions.
RECOVERY_KERNEL = build/rach2-recovery/kernel
rach2-recovery: krffs rach2
	$(MAKE) -C rach2 COMPONENT=rach2-recovery KEXTRA=-DRACH2_RECOVERY
	python3 tools/inspect/check_recovery.py $(RECOVERY_KERNEL) recovery
	python3 tools/inspect/check_recovery.py build/rach2/kernel ordinary
	python3 tools/inspect/check_diskpolicy.py $(RECOVERY_KERNEL) read-write

rach2-metal-image: krffs rach2 rach2-metal utils check-busybox check-setid check-dropbearmulti
	sh krffs/gen_demo_key.sh
	sh krffs/gen_demo_password.sh
	$(MKIMAGE) krffs/manifests/rach2-metal.manifest $(METAL_IMAGE) 4

# The recovery RAM image: what the recovery kernel loads as its root. It is the
# metal profile plus the two programs that repair a disk, and it mounts nothing
# from the disk it is repairing, which is what lets it rewrite that disk's root.
RECOVERY_IMAGE = $(IMAGES_DIR)/rach2-recovery.krffs
rach2-recovery-image: krffs rach2 rach2-recovery utils check-busybox check-setid check-dropbearmulti
	sh krffs/gen_demo_key.sh
	sh krffs/gen_demo_password.sh
	$(MKIMAGE) krffs/manifests/rach2-recovery.manifest $(RECOVERY_IMAGE) 4

# The os-real disk: the same os-real tree, but on a real disk image instead of a
# RAM-disk module. A 64 MB disk with a 48 MB KRFFS partition at LBA 2048; the
# partition is larger than the 32 MB resident page-cache cap on purpose, so it can
# later prove an on-disk file system bigger than what fits in RAM at once.
os-real-disk: krffs rach2 utils check-busybox check-tcc check-pdpmake check-setid check-dropbearmulti ports/doom
	sh krffs/gen_demo_key.sh
	sh krffs/gen_demo_password.sh
	$(MKDISK) krffs/manifests/os-real.manifest $(OS_REAL_DISK) 64 48

# A BIOS-bootable disk: GRUB in the MBR and a FAT boot partition holding the
# kernel, plus a KRFFS partition with the os-real tree. It boots a real machine
# (or QEMU with no -kernel/-initrd) BIOS -> MBR -> GRUB -> kernel -> KRFFS on the
# disk. Built in a throwaway Docker container (needs Docker).
os-real-grub-disk: krffs rach2 utils check-busybox check-tcc check-pdpmake check-setid check-dropbearmulti ports/doom
	sh krffs/gen_demo_key.sh
	sh krffs/gen_demo_password.sh
	sh tools/image/mkdisk_grub.sh

# `make rach2-metal-grub-disk`: the disk that goes INTO the laptop, imaged over
# a USB adapter and swapped in place of the Windows disk. The full os-real
# machine (/src, the compilers, the installer) with the metal init supervising
# dropbear, the LAN address and the AHCI driver named in every menu entry, and
# a KRFFS partition sized for real work rather than for a test. The kernel is
# the ordinary read-write build, which is safe for one reason only: this image
# is written to a disk the project owns, and it boots in the laptop only after
# the Windows disk is physically out.
METAL_GRUB_DISK = build/images/rach2-metal-grub.disk
rach2-metal-grub-disk: krffs rach2 utils check-busybox check-tcc check-pdpmake check-setid check-dropbearmulti ports/doom
	sh krffs/gen_demo_key.sh
	sh krffs/gen_demo_password.sh
	RACH2_DISK_MANIFEST=krffs/manifests/rach2-metal-disk.manifest \
	RACH2_DISK_KRFFS_MB=500 \
	RACH2_DISK_APPEND="rach2.disk=ahci rach2.ip=192.168.0.105/24 rach2.gateway=192.168.0.1 rach2.netconsole" \
	    sh tools/image/mkdisk_grub.sh build/rach2/kernel $(METAL_GRUB_DISK)

# `make rach2-sealed-disk`: the disk milestone 25 seals into the laptop, written
# once over a USB adapter and not removed again. Its geometry is the milestone's
# contract and every number here is checked against the built image: a 64 MiB
# FAT32 boot partition at 4 KiB clusters (one sector per cluster puts a 64 MiB
# volume past both of bootslot's bounds), a 500 MiB root, and a 64 MiB recovery
# partition whose kernel and RAM image GRUB can reach when partition 1 cannot be
# read at all. The image is 630 MiB, 1290240 sectors.
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
# slots and the recovery contents are added by the phases that own them.
# The fixed address the two static entries carry, for the day the network this
# machine is plugged into has no server: the same defaults the metal stick uses,
# overridable at build time for anywhere else. The menu takes a prefix and the
# recovery script takes a dotted mask, and the two must be the same subnet with
# the gateway inside it; tools/inspect/check_sealednet.py holds them to that before a
# disk is written, since a machine whose menu and fallback disagree is one
# nobody can reach by either.
SEALED_ADDRESS ?= 192.168.0.105
SEALED_MASK ?= 255.255.255.0
SEALED_PREFIX ?= 24
SEALED_GATEWAY ?= 192.168.0.1
SEALED_IP = $(SEALED_ADDRESS)/$(SEALED_PREFIX)
SEALED_DISK = build/images/rach2-sealed.disk
rach2-sealed-disk: krffs rach2 utils rach2-recovery rach2-recovery-image check-busybox check-tcc check-pdpmake check-setid check-dropbearmulti ports/doom
	sh krffs/gen_demo_key.sh
	sh krffs/gen_demo_password.sh
	@python3 tools/inspect/check_sealednet.py $(SEALED_ADDRESS) $(SEALED_PREFIX) \
	    $(SEALED_MASK) $(SEALED_GATEWAY) krffs/rescue-static.script
	RACH2_DISK_MANIFEST=krffs/manifests/rach2-metal-disk.manifest \
	RACH2_DISK_BOOT_MB=64 \
	RACH2_DISK_CLUSTER=8 \
	RACH2_DISK_KRFFS_MB=500 \
	RACH2_DISK_RECOVERY_MB=64 \
	RACH2_DISK_APPEND="rach2.disk=ahci rach2.netconsole" \
	RACH2_DISK_DHCP_APPEND="rach2.ip=none" \
	RACH2_DISK_STATIC_APPEND="rach2.ip=$(SEALED_IP) rach2.gateway=$(SEALED_GATEWAY)" \
	RACH2_DISK_RECOVERY_KERNEL=$(RECOVERY_KERNEL) \
	RACH2_DISK_RECOVERY_IMAGE=$(RECOVERY_IMAGE) \
	    sh tools/image/mkdisk_grub.sh build/rach2/kernel $(SEALED_DISK)

# `make test`: the automated end-to-end pass over the test image. Boots it headless
# (-cpu max) and runs every probe, failing if any does not exit 0. The few probes
# with a special contract (fork-exit-code, stdin, faults, network) are run by hand
# under tests/system/runner/harness.py; see tests/system/runner/runtests.py.
# --timeout is one deadline for the whole run, not per probe, so it is stated here
# rather than left at the default: the suite grows every milestone, and a default
# sized for an older, shorter suite fails the tail as a hang on a slow host.
# --steptmo is the per-probe window, and it is stated here for one probe:
# musl/pwauthmusl waits out the credential lock's real ten-second retry budget
# twice, so tens of seconds is its GREEN path. Only a failing probe ever waits a
# step window out, so widening it costs a passing run nothing.
# The default suite's build-and-run target. `make test` and `tests/run
# default` both land here; the registry in tests/registry names it.
run-default: test-image
	python3 tests/system/runner/runtests.py --kernel build/rach2/kernel --initrd $(TEST_IMAGE) \
	    --timeout 300 --steptmo 60

# The public test entry: the runner owns the suite registry, so this is a
# shortcut for `tests/run default` and holds no suite knowledge of its own.
test:
	@./tests/run default

# `make test-addressed`: the same suite, with the network named on the boot line
# instead of compiled in. The address given is the one SLIRP already provides,
# so every probe must pass exactly as it does without it; what is under test is
# that `rach2.ip=` and `rach2.gateway=` reach ARP, IP, TCP, the sockets and the
# boot line, and that naming the current network changes nothing.
run-addressed: test-image
	python3 tests/system/runner/runtests.py --kernel build/rach2/kernel --initrd $(TEST_IMAGE) \
	    --timeout 300 --steptmo 60 \
	    --append "rach2.ip=10.0.2.15/24 rach2.gateway=10.0.2.2"

# `make test-noentropy`: the salt policy's negative half, which no other target
# can reach. `make test` always boots -cpu max, so the CPU has RDSEED and RDRAND
# and the kernel's pool is trusted; a minter that quietly fell back to the
# untrusted stream would pass every proof in the suite. This boots the same test
# image on a CPU that exposes neither instruction, and /musl/saltnoent (staged by
# the image sweep, deliberately absent from runtests.py's PROBES because it must
# fail where entropy is trusted) asserts that the mint refuses, says so once, and
# leaves /etc/shadow, its temporary file and the lock exactly as they were.
# harness.py passes no -cpu of its own, so the passthrough is the only one on the
# command line.
run-noentropy: test-image
	python3 tests/system/runner/harness.py --kernel build/rach2/kernel --initrd $(TEST_IMAGE) \
	    --qemu-arg=-cpu --qemu-arg=qemu64 \
	    --timeout 90 --ok "SALTNOENT OK" \
	    "expect=Type 'help'" "send=exec /musl/saltnoent" wait=20
	python3 tests/system/runner/harness.py --kernel build/rach2/kernel --initrd $(TEST_IMAGE) \
	    --qemu-arg=-cpu --qemu-arg=qemu64 \
	    --timeout 180 --ok "PASSWDMUSL OK (fail-closed)" \
	    "expect=Type 'help'" "send=exec /musl/passwdmusl" wait=60

# `make test-disk`: the disk-backed page-cache integrity check. Builds a disk
# image holding blockprobe, boots disk-only with KRFFS on the real ATA disk
# through the cache (rach2.fsdisk), and runs blockprobe from the debug shell,
# passing on its "BLOCKPROBE OK" serial token. blockprobe writes more than the
# resident cache, so it exercises real eviction and writeback and fails on any
# dropped or corrupted page.
DISK_TEST_IMAGE = $(IMAGES_DIR)/disk-test.disk
run-disk: rach2 tests krffs
	$(MKDISK) krffs/manifests/disk-test.manifest $(DISK_TEST_IMAGE) 64 48
	python3 tests/system/runner/harness.py --kernel build/rach2/kernel \
	    --append "rach2.fsdisk rach2.rescue" \
	    --drive "file=$(DISK_TEST_IMAGE),format=raw,if=ide,index=0,media=disk" \
	    --timeout 90 --ok "BLOCKPROBE OK" probe=blockprobe

# `make bootpart-test`: /proc/cmdline and the level-one disk devices. Three
# boots of one disk: as mkdisk built it there is a single KRFFS partition, so
# /dev/bootpart must be absent from stat and open; after tests/system/fixtures/mkbootfixture.py
# adds a FAT-typed partition of known bytes, the probe drives the device end to
# end (the 0600 mode from both stat paths, seeks against the partition size,
# known bytes at a seek, a written sector read back, exactly 0 at the end,
# -ENOSPC past it, -EINVAL unaligned, -EACCES unprivileged); and a third boot
# mounts a RAM image instead of the disk and requires both /dev/bootpart and
# /dev/grubcore anyway. That third one is the milestone 25 property: a machine
# that mounts nothing from its disk still manages its boot partition and its
# boot loader, which is what recovery depends on. A fourth boot attaches a
# sparse 260 GB drive over AHCI and reads known sectors at 5 GiB and at 258
# GiB, which the raw windows could not address at all while an open file's
# offset was 32 bits. It is that large on purpose: a few gigabytes would be
# satisfied by an implementation that kept 33 bits, and it is attached over
# AHCI because the ATA driver's 28-bit LBA stops at 128 GiB.
BOOTPART_TEST_IMAGE = $(IMAGES_DIR)/bootpart-test.disk
WIDE_TEST_IMAGE = $(IMAGES_DIR)/wide-test.disk
run-bootpart: rach2 tests krffs
	$(MKDISK) krffs/manifests/disk-test.manifest $(BOOTPART_TEST_IMAGE) 64 48
	python3 tests/system/runner/harness.py --kernel build/rach2/kernel \
	    --append "rach2.fsdisk rach2.rescue" \
	    --drive "file=$(BOOTPART_TEST_IMAGE),format=raw,if=ide,index=0,media=disk" \
	    --timeout 90 --ok "BOOTPROBE ABSENT OK" probe=bootprobe
	python3 tests/system/fixtures/mkbootfixture.py $(BOOTPART_TEST_IMAGE)
	python3 tests/system/runner/harness.py --kernel build/rach2/kernel \
	    --append "rach2.fsdisk rach2.rescue rach2.bootprobe=fixture" \
	    --drive "file=$(BOOTPART_TEST_IMAGE),format=raw,if=ide,index=0,media=disk" \
	    --timeout 90 --ok "BOOTPROBE OK" probe=bootprobe
	python3 tests/system/runner/harness.py --kernel build/rach2/kernel --initrd $(TEST_IMAGE) \
	    --append "rach2.rescue rach2.bootprobe=ramroot" \
	    --drive "file=$(BOOTPART_TEST_IMAGE),format=raw,if=ide,index=0,media=disk" \
	    --timeout 90 --ok "BOOTPROBE RAMROOT OK" probe=bootprobe
	python3 tests/system/fixtures/mkwidefixture.py $(WIDE_TEST_IMAGE)
	python3 tests/system/runner/harness.py --kernel build/rach2/kernel --initrd $(TEST_IMAGE) \
	    --append "rach2.disk=ahci rach2.rescue rach2.diskdiag rach2.bootprobe=wide" \
	    --drive "file=$(WIDE_TEST_IMAGE),format=raw,if=none,id=drv0" \
	    $(AHCI_ATTACH) \
	    --timeout 180 --ok "BOOTPROBE WIDE OK" probe=bootprobe

# `make grub-test`: the machine replacing its own boot loader. The post-MBR gap
# holds two core-image tails and LBA 1 selects between them, so a replacement
# writes the tail not in use, reads it back, and only then rewrites that one
# sector. Sector 0, which carries the partition table, is never written.
#
# Two core images are built with a marker embedded in each and staged into the
# root through os-real's optional-file lines, then installed in turn; every boot's marker must be the image the boot
# before it installed. The marker comes from inside the running core, because
# reading the pointer back would prove only bytes at rest.
GRUB_TEST_DISK = $(IMAGES_DIR)/grub-test.disk
run-grub: rach2 utils krffs check-busybox check-tcc check-pdpmake check-setid check-dropbearmulti ports/doom
	@mkdir -p $(IMAGES_DIR)
	sh tools/image/mkgrubcore.sh A $(IMAGES_DIR)/core-a.img
	sh tools/image/mkgrubcore.sh B $(IMAGES_DIR)/core-b.img
	sed -e '/^#/d' -e '/^$$/d' -e '\|^file 0644 /core-[ab]\.img|d' \
	    krffs/manifests/grub-test.manifest > $(IMAGES_DIR)/grub-test.manifest.stripped
	sed -e '/^#/d' -e '/^$$/d' krffs/manifests/os-real.manifest \
	    | diff - $(IMAGES_DIR)/grub-test.manifest.stripped
	sh krffs/gen_demo_password.sh
	sh krffs/gen_demo_key.sh
	RACH2_DISK_MANIFEST=krffs/manifests/grub-test.manifest \
	    sh tools/image/mkdisk_grub.sh build/rach2/kernel $(GRUB_TEST_DISK)
	sh tests/system/suites/grub.sh $(GRUB_TEST_DISK) \
	    "`cat krffs/rach2_demo_password`"

# `make menu-test`: the boot menu that can be rewritten, and the stub that
# cannot. Four boots of one GRUB disk: a two-entry menu staged through `bootctl
# config` with a grubenv key round-tripped and the candidate kernel corrupted;
# a boot proving both staged entries reached the menu and that the fallback
# NUMBER still named the stub's own entry rather than a staged one; that file
# truncated mid-entry from the host, after which the machine must still boot
# through the stub with GRUB's parse error in the log; and the slot's extent
# read back to prove bootctl overwrote bytes in place rather than rewriting a
# file. Docker absent fails the run: this is the phase's proof.
MENU_TEST_DISK = $(IMAGES_DIR)/menu-test.disk
run-menu: rach2 utils krffs check-busybox check-tcc check-pdpmake check-setid check-dropbearmulti ports/doom
	@mkdir -p $(IMAGES_DIR)
	sh krffs/gen_demo_password.sh
	sh krffs/gen_demo_key.sh
	sh tools/image/mkdisk_grub.sh build/rach2/kernel $(MENU_TEST_DISK)
	sh tests/system/suites/menu.sh $(MENU_TEST_DISK) \
	    "`cat krffs/rach2_demo_password`"

# `make authority-test`: which kernel may write the whole disk. Two boots of one
# probe against one sparse image, differing only in the kernel. The everyday
# build must refuse a write to /dev/disk0 with -EPERM and the sector must still
# be zero afterwards, so the refusal is proved to have written nothing rather
# than merely returned an error. The recovery build must write a sector past
# 2 GiB, read it back byte for byte, and take a 200-sector write in one call,
# which is what makes a whole-disk reflash finishable: one sector per driver
# call turned a 500 MiB write into a million cache flushes.
#
# The two kernels are the same sources built with different switches into
# different object trees, and check_recovery.py reads the authority back off
# each artifact so a mislabelled one cannot reach this test.
AUTH_TEST_IMAGE = $(IMAGES_DIR)/auth-test.disk
run-authority: rach2 rach2-recovery tests krffs test-image
	python3 tests/system/fixtures/mkwidefixture.py $(AUTH_TEST_IMAGE)
	python3 tools/inspect/check_recovery.py build/rach2/kernel ordinary
	python3 tools/inspect/check_recovery.py $(RECOVERY_KERNEL) recovery
	python3 tests/system/runner/harness.py --kernel build/rach2/kernel --initrd $(TEST_IMAGE) \
	    --append "rach2.disk=ahci rach2.rescue rach2.diskdiag rach2.bootprobe=auth-ordinary" \
	    --drive "file=$(AUTH_TEST_IMAGE),format=raw,if=none,id=drv0" \
	    $(AHCI_ATTACH) \
	    --timeout 180 --ok "BOOTPROBE AUTH ORDINARY OK" probe=bootprobe
	python3 tests/system/runner/harness.py --kernel $(RECOVERY_KERNEL) --initrd $(TEST_IMAGE) \
	    --append "rach2.disk=ahci rach2.rescue rach2.diskdiag rach2.bootprobe=auth-recovery" \
	    --drive "file=$(AUTH_TEST_IMAGE),format=raw,if=none,id=drv0" \
	    $(AHCI_ATTACH) \
	    --timeout 180 --ok "BOOTPROBE AUTH RECOVERY OK" probe=bootprobe

# `make install-test`: the machine installs its own kernel. Builds the GRUB
# disk around the host kernel, boots it with no -kernel, and runs the whole
# install loop twice as root: a marker edited into the banner source, the
# kernel built in place, installed as a one-boot trial, proved running by its
# marker after reboot -f, committed, held across a cold boot, corrupted at
# the installer-named extent, recovered through the fallback and repaired
# from inside. The interruption matrix's determinate states are then booted
# from host-crafted environment blocks. Docker required; its absence fails
# the target, because this harness is the milestone's proof.
INSTALL_TEST_DISK = $(IMAGES_DIR)/install-test.disk
run-install: rach2 utils krffs check-busybox check-tcc check-pdpmake check-setid check-dropbearmulti ports/doom
	@mkdir -p $(IMAGES_DIR)
	sh krffs/gen_demo_password.sh
	sh krffs/gen_demo_key.sh
	sh tools/image/mkdisk_grub.sh build/rach2/kernel $(INSTALL_TEST_DISK)
	sh tests/system/suites/install.sh $(INSTALL_TEST_DISK) \
	    "`cat krffs/rach2_demo_password`"

# `make sealed-test`: the disk that goes into the laptop, operated the way it
# will be operated once it is in there. Two runs of one artifact on separate
# copies: a fault run that walks the whole life of the machine over SSH (a
# kernel sent down the connection, installed, tried and committed; a destroyed
# candidate caught by the fallback and repaired; a menu staged and then torn; the
# boot loader replaced with a marked core; the root destroyed and rewritten from
# recovery), and a control run that injects nothing and asserts that no fallback
# and no recovery appears anywhere. Docker builds the artifact; the runs are
# QEMU and ssh.
run-sealed: rach2-sealed-disk $(DEADLINE)
	sh tests/system/suites/sealed.sh $(SEALED_DISK) \
	    "`cat krffs/rach2_demo_password`" 2297

# `make drill-test`: the recovery drill, boots of one disk that carries the same
# three partitions the sealed disk has. The everyday kernel must refuse to write
# its own disk and be shown to have written nothing; a root destroyed with the
# machine stopped must stop the boot; the recovery entry must come up from
# partition 3, take an address and answer SSH with nothing typed at the console;
# the whole root partition must arrive through one `ssh` pipe and hash
# identically when read back through the same device; the machine must then boot
# from it; a recovery payload that will not load must fall back to partition 1;
# and a disk whose boot partition is destroyed must still reach recovery through
# the core image's own configuration. The disk is small so the drill is quick.
#
# Its entries carry `rach2.diskdiag`, which is what publishes /dev/disk0 on an
# everyday kernel: the refusal is worth proving on a machine that HAS the device
# rather than on one where the node is simply absent. The sealed disk carries no
# such flag, so there the everyday entries have no whole-disk node at all, and
# the recovery build publishes its own whatever the command line says.
DRILL_DISK = $(IMAGES_DIR)/drill-test.disk
run-drill: rach2 rach2-recovery rach2-recovery-image utils krffs check-setid
	@mkdir -p $(IMAGES_DIR)
	sh krffs/gen_demo_password.sh
	sh krffs/gen_demo_key.sh
	RACH2_DISK_MANIFEST=krffs/manifests/os-simple.manifest \
	RACH2_DISK_KRFFS_MB=16 \
	RACH2_DISK_RECOVERY_MB=8 \
	RACH2_DISK_APPEND="rach2.diskdiag" \
	RACH2_DISK_DHCP_APPEND="rach2.ip=none" \
	RACH2_DISK_RECOVERY_KERNEL=$(RECOVERY_KERNEL) \
	RACH2_DISK_RECOVERY_IMAGE=$(RECOVERY_IMAGE) \
	    sh tools/image/mkdisk_grub.sh build/rach2/kernel $(DRILL_DISK)
	sh tests/system/suites/drill.sh $(DRILL_DISK) \
	    "`cat krffs/rach2_demo_password`" 2298

# `make persist-test`: fsync durability across a reboot. Builds a disk holding
# persistprobe, boots it twice against the SAME image: the first boot writes and
# fsyncs /persist.txt, the second reads it back. The second boot passes only if
# fsync pushed the write through the cache to the disk, since the file would
# otherwise be gone.
PERSIST_DISK = $(IMAGES_DIR)/persist-test.disk
run-persist: rach2 tests krffs
	$(MKDISK) krffs/manifests/persist-test.manifest $(PERSIST_DISK) 64 48
	python3 tests/system/runner/harness.py --kernel build/rach2/kernel \
	    --append "rach2.fsdisk rach2.rescue" \
	    --drive "file=$(PERSIST_DISK),format=raw,if=ide,index=0,media=disk" \
	    --timeout 60 --ok "PERSISTPROBE OK (wrote)" probe=persistprobe
	python3 tests/system/runner/harness.py --kernel build/rach2/kernel \
	    --append "rach2.fsdisk rach2.rescue" \
	    --drive "file=$(PERSIST_DISK),format=raw,if=ide,index=0,media=disk" \
	    --timeout 60 --ok "PERSISTPROBE OK (read back)" probe=persistprobe

# `make cred-persist-test`: the credential replacement's durability across a
# reboot. Both of cred_rewrite's flushes are no-ops on the RAM module `make test`
# boots, so a library that dropped either would pass every other proof in the
# milestone. This boots one disk image three times. The first boot publishes one
# replacement and releases, then publishes a second and is cut off holding the
# lock, as a power failure would cut it off. The second boot reads both back with
# the modes and owners they inherited, and reports the lock the cut-off writer
# left rather than breaking it. The third reads back the release itself: a lock
# whose removal stayed in the page cache would still be there.
CRED_PERSIST_DISK = $(IMAGES_DIR)/cred-persist-test.disk
run-cred-persist: rach2 musl-programs krffs
	$(MKDISK) krffs/manifests/cred-persist-test.manifest $(CRED_PERSIST_DISK) 64 48
	python3 tests/system/runner/harness.py --kernel build/rach2/kernel \
	    --append "rach2.fsdisk rach2.rescue" \
	    --drive "file=$(CRED_PERSIST_DISK),format=raw,if=ide,index=0,media=disk" \
	    --timeout 60 --ok "CREDDISK OK (wrote)" \
	    "expect=Type 'help'" "send=exec /creddisk" wait=15
	python3 tests/system/runner/harness.py --kernel build/rach2/kernel \
	    --append "rach2.fsdisk rach2.rescue" \
	    --drive "file=$(CRED_PERSIST_DISK),format=raw,if=ide,index=0,media=disk" \
	    --timeout 60 --ok "CREDDISK OK (released)" \
	    "expect=Type 'help'" "send=exec /creddisk" wait=15
	python3 tests/system/runner/harness.py --kernel build/rach2/kernel \
	    --append "rach2.fsdisk rach2.rescue" \
	    --drive "file=$(CRED_PERSIST_DISK),format=raw,if=ide,index=0,media=disk" \
	    --timeout 60 --ok "CREDDISK OK (read back)" \
	    "expect=Type 'help'" "send=exec /creddisk" wait=15

# `make crash-test`: prove the dirty-mount flag detects an unclean shutdown. A
# crashed boot (killed after mounting) leaves the on-disk dirty bit set for
# fsck.krffs to report; a clean halt clears it. Its own image so it cannot race
# test-disk on a shared file under `make -j`.
CRASH_TEST_IMAGE = $(IMAGES_DIR)/crash-test.disk
# The capture `make raw-test` reads the hop limits and the checksum out of.
RAW_PCAP = $(IMAGES_DIR)/raw.pcap
# Its own disk image, so `make -j` cannot have this target and `test-disk`
# rewriting one file while the other boots it.
BOOTLINE_TEST_IMAGE = $(IMAGES_DIR)/bootline-test.disk
# `make ping-test`: the ICMP path against the SLIRP gateway, which is why it is
# its own target rather than a probe in the suite. The explicit wait= is
# required: after its last step harness.py drains only --grace and then
# kills QEMU, while the timeout case alone costs four seconds and the child the
# probe forks lives three to eight, so without it the token would arrive after
# QEMU was dead.
run-ping: rach2 tests krffs test-image
	python3 tests/system/runner/harness.py --kernel build/rach2/kernel \
	    --initrd $(TEST_IMAGE) --qemu-arg=-cpu --qemu-arg=max \
	    --timeout 120 --ok "PINGTEST OK" \
	    "expect=Type 'help'" "send=exec /pingtest" wait=60

# `make packet-test`: the packet socket, which is how a DHCP client sees a reply
# that arrives before this machine owns an address. Its own target and not a
# probe in the suite, for the reason `ping-test` is: it needs the network to
# answer, and the suite's probes must not.
run-packet: rach2 tests krffs test-image
	python3 tests/system/runner/harness.py --kernel build/rach2/kernel \
	    --initrd $(TEST_IMAGE) --qemu-arg=-cpu --qemu-arg=max \
	    --timeout 120 --ok "PACKETPROBE OK" \
	    "expect=Type 'help'" "send=exec /packetprobe" wait=60

# `make bootline-test`: the boot line that contradicts itself. `rach2.nodisk`
# skips the disk an image rooted on it needs, and the machine would fall to a
# shell that authenticates nobody, so the kernel refuses that combination; with
# `rach2.rescue` it is allowed, since asking for that shell is what the token is
# for. Four boots, and each refusal is asserted twice: the diagnostic, and that
# no shell was reached after it.
run-bootline: rach2 tests krffs
	$(MKDISK) krffs/manifests/disk-test.manifest $(BOOTLINE_TEST_IMAGE) 64 48
	sh tests/system/suites/bootline.sh build/rach2/kernel $(BOOTLINE_TEST_IMAGE)

# `make raw-test`: the raw ICMP socket, which is what `ping` and `traceroute`
# are. Three boots, because the three things it must show cannot be shown in
# one: the probe with the network answering, the same probe with an address and
# no gateway (where the only truthful answer is that there is no route), and
# `traceroute` itself on the image that has it. The capture is read by
# tests/system/checkers/rawwire_check.py for the two things a program inside the machine cannot
# see: the hop limit each datagram left with, and the caller's own checksum.
run-raw: rach2 tests krffs test-image os-real-image
	@rm -f $(RAW_PCAP)
	python3 tests/system/runner/harness.py --kernel build/rach2/kernel \
	    --initrd $(TEST_IMAGE) --qemu-arg=-cpu --qemu-arg=max \
	    --qemu-arg=-object \
	    --qemu-arg=filter-dump,id=rw0,netdev=net0,file=$(RAW_PCAP) \
	    --timeout 200 --ok "RAWPROBE OK" \
	    "expect=Type 'help'" "send=exec /rawprobe" wait=60
	python3 tests/system/checkers/rawwire_check.py $(RAW_PCAP)
	python3 tests/system/runner/harness.py --kernel build/rach2/kernel \
	    --initrd $(TEST_IMAGE) --qemu-arg=-cpu --qemu-arg=max \
	    --append "rach2.ip=10.0.2.15/24" \
	    --timeout 200 --ok "RAWPROBE OK" \
	    "expect=Type 'help'" "send=exec /rawprobe" wait=30
	# And the program the socket was built for. The ping first is not a
	# flourish: a datagram for an address whose hardware address is not known
	# yet is dropped while the request goes out, and traceroute sends one
	# probe per hop and does not retry, so a cold cache costs it its first
	# answer. What is asserted is the first hop, which is the gateway
	# answering a hop limit of one, and that is the whole mechanism.
	python3 tests/system/runner/harness.py --kernel build/rach2/kernel \
	    --initrd $(OS_REAL_IMAGE) --qemu-arg=-cpu --qemu-arg=max \
	    --timeout 300 --steptmo 90 --ok "TRACEDONE" \
	    "expect=rach2 login:" "send=root" "expect=Password:" \
	    "send=`cat krffs/rach2_demo_password`" \
	    "expect=# " \
	    "send=/bin/ping 10.0.2.2 > /dev/null; traceroute -n -q 1 -m 2 8.8.8.8" \
	    "expect= 1  10.0.2.2" "send=echo TRACE\"\"DONE" wait=10
	# The in-house ping is still the one an ordinary account runs: busybox's
	# needs a raw socket, which is root's, and is not built for that reason.
	python3 tests/system/runner/harness.py --kernel build/rach2/kernel \
	    --initrd $(OS_REAL_IMAGE) --qemu-arg=-cpu --qemu-arg=max \
	    --timeout 300 --steptmo 90 --ok "USERPINGOK" \
	    "expect=rach2 login:" "send=test" "expect=Password:" \
	    "send=`cat krffs/rach2_test_password`" wait=5 \
	    "send=/bin/ping 10.0.2.2 && echo USERPING\"\"OK" wait=15

# `make netif-test`: the interface ioctls, which is how a program sets this
# machine's address. Its own target, like `ping-test`: it changes the address
# out from under the machine and puts it back, and the last thing it does is
# ping the gateway, which needs the network to answer.
run-netif: rach2 tests krffs test-image
	python3 tests/system/runner/harness.py --kernel build/rach2/kernel \
	    --initrd $(TEST_IMAGE) --qemu-arg=-cpu --qemu-arg=max \
	    --net "user,id=net0,hostfwd=udp::15999-:9999" \
	    --timeout 180 --ok "NETIFPROBE OK" \
	    "expect=Type 'help'" "send=exec /netifprobe" \
	    "expect=NETIFPROBE DOWN" "udp=15999:down" \
	    "expect=NETIFPROBE UP" "udp=15999:up" wait=60
	python3 tests/system/runner/harness.py --kernel build/rach2/kernel \
	    --initrd $(TEST_IMAGE) --qemu-arg=-cpu --qemu-arg=max \
	    --append "rach2.ip=none" \
	    --timeout 120 --ok "NETIFPROBE OK" \
	    "expect=Type 'help'" "send=exec /netifprobe" wait=60

# `make dgram-test`: a connected datagram socket and the permission to
# broadcast, which is how a DHCP client renews a lease and how it asks for one.
# Its own target because it broadcasts, and a probe in the suite must not put a
# frame on the wire that the other probes did not ask for.
DGRAM_PCAP = $(IMAGES_DIR)/dgram.pcap
run-dgram: rach2 tests krffs test-image
	@rm -f $(DGRAM_PCAP)
	python3 tests/system/runner/harness.py --kernel build/rach2/kernel \
	    --initrd $(TEST_IMAGE) --qemu-arg=-cpu --qemu-arg=max \
	    --net "user,id=net0,hostfwd=udp::15998-:7502" \
	    --qemu-arg=-object \
	    --qemu-arg=filter-dump,id=dg0,netdev=net0,file=$(DGRAM_PCAP) \
	    --timeout 180 --ok "DGRAMPROBE OK" \
	    "expect=Type 'help'" "send=exec /dgramprobe" \
	    "expect=DGRAMPROBE OUTSIDE" "udp=15998:outsider" wait=30
	python3 tests/system/checkers/bcast_check.py $(DGRAM_PCAP) --port 7500 --dst-ip 255.255.255.255

# `make dhcp-test`: the machine taking an address it was not told, with the
# client os-real carries. It boots with `rach2.ip=none`, so IPv4 is unset and
# nothing is configured until userland does it, and QEMU's user network is
# given a network of its own so the address that arrives cannot be the one the
# machine would have had anyway.
DHCP_NET = user,id=net0,net=192.168.90.0/24,host=192.168.90.2,dhcpstart=192.168.90.20,dns=192.168.90.3
DHCP_PCAP = $(IMAGES_DIR)/dhcp.pcap
DHCP_DISK = $(IMAGES_DIR)/dhcp-test.disk
METAL_PCAP = $(IMAGES_DIR)/dhcp-metal.pcap
SIMPLE_PCAP = $(IMAGES_DIR)/dhcp-simple.pcap
SIMPLE_LOG = $(IMAGES_DIR)/dhcp-simple.log
STATIC_PCAP = $(IMAGES_DIR)/static-simple.pcap
RSTATIC_PCAP = $(IMAGES_DIR)/static-real.pcap
SIMPLE_NET = user,id=net0,net=10.90.0.0/16,host=10.90.0.2,dhcpstart=10.90.0.20,dns=10.90.0.3
run-dhcp: rach2 utils busybox krffs os-real-image os-simple-image
	@rm -f $(DHCP_PCAP)
	python3 tests/system/runner/harness.py --kernel build/rach2/kernel \
	    --initrd $(OS_REAL_IMAGE) --qemu-arg=-cpu --qemu-arg=max \
	    --net "$(DHCP_NET)" \
	    --qemu-arg=-object \
	    --qemu-arg=filter-dump,id=dh0,netdev=net0,file=$(DHCP_PCAP) \
	    --append "rach2.ip=none" \
	    --timeout 240 --steptmo 60 --ok "DHCPOK" --log $(IMAGES_DIR)/dhcp.log \
	    "expect=rach2 login:" "send=root" "expect=Password:" \
	    "send=`cat krffs/rach2_demo_password`" \
	    "expect=# " "send=ifconfig eth0 | grep -q 'addr:0.0.0.0' && echo UNSET\"\"OK" \
	    "expect=UNSETOK" "send=ifup -a" \
	    "expect=lease of 192.168.90.20 obtained" wait=3 \
	    "send=ifconfig eth0 > /tmp/if; grep -q 'addr:192.168.90.20' /tmp/if && grep -q 'Bcast:192.168.90.255' /tmp/if && grep -q 'Mask:255.255.255.0' /tmp/if && grep -q '192.168.90.3' /etc/resolv.conf && ping 192.168.90.2 > /dev/null && echo LEASE\"\"OK" \
	    "expect=LEASEOK" \
	    "send=arping -f -w 5 192.168.90.2 > /tmp/ap 2>&1; echo ARPING\"\"-STATUS:$$?; cat /tmp/ap" \
	    "expect=ARPING-STATUS:0" "expect=Received 1 response" \
	    "send=arp -n | grep -q '(192.168.90.2)' && echo ARP\"\"CACHED" \
	    "expect=ARPCACHED" \
	    "send=ping 10.9.9.9 > /dev/null 2>&1; echo ROUTE\"\"TRIED" \
	    "expect=ROUTETRIED" "send=echo DHCP\"\"OK" \
	    wait=10
	# The address, the netmask and the broadcast are asserted above, and the
	# route below: a packet for an address on no subnet of this machine's must
	# leave addressed to the gateway's hardware address, which is what a
	# default route means. Nothing answers 10.9.9.9, and nothing needs to.
	python3 tests/system/checkers/bcast_check.py $(DHCP_PCAP) \
	    --routed-via 192.168.90.2 --routed-dst 10.9.9.9
	# And twice on a disk, where /var/run survives a reboot. `ifup` records
	# there which interfaces it has configured, and the second boot must take
	# a lease rather than read the first boot's record and decide it is
	# already configured. Init empties that directory, and this is what says
	# so: two boots of one disk, each of which must reach a lease.
	$(MKDISK) krffs/manifests/os-real.manifest $(DHCP_DISK) 64 48
	python3 tests/system/runner/harness.py --kernel build/rach2/kernel \
	    --append "rach2.fsdisk rach2.ip=none" \
	    --drive "file=$(DHCP_DISK),format=raw,if=ide,index=0,media=disk" \
	    --qemu-arg=-cpu --qemu-arg=max --net "$(DHCP_NET)" \
	    --timeout 240 --steptmo 60 --ok "FIRSTLEASE" \
	    "expect=rach2 login:" "send=root" "expect=Password:" \
	    "send=`cat krffs/rach2_demo_password`" \
	    "expect=# " "send=ifup -a" \
	    "expect=lease of 192.168.90.20 obtained" wait=3 \
	    "send=ifconfig eth0 | grep -q 'addr:192.168.90.20' && echo FIRST\"\"LEASE" \
	    "expect=FIRSTLEASE" "send=poweroff -f" wait=5
	python3 tests/system/runner/harness.py --kernel build/rach2/kernel \
	    --append "rach2.fsdisk rach2.ip=none" \
	    --drive "file=$(DHCP_DISK),format=raw,if=ide,index=0,media=disk" \
	    --qemu-arg=-cpu --qemu-arg=max --net "$(DHCP_NET)" \
	    --timeout 240 --steptmo 60 --ok "SECONDLEASE" \
	    "expect=rach2 login:" "send=root" "expect=Password:" \
	    "send=`cat krffs/rach2_demo_password`" \
	    "expect=# " "send=ifup -a" \
	    "expect=lease of 192.168.90.20 obtained" wait=3 \
	    "send=ifconfig eth0 | grep -q 'addr:192.168.90.20' && echo SECOND\"\"LEASE" \
	    "expect=SECONDLEASE" "send=poweroff -f" wait=5
	# And unattended, which is how the sealed machine comes up: nothing is
	# typed at all. The metal init runs `ifup -a` itself, after the console
	# login and before the SSH server, so the lease must arrive with no
	# console session in it.
	$(MKIMAGE) krffs/manifests/rach2-metal.manifest $(METAL_IMAGE) 4
	@rm -f $(METAL_PCAP)
	python3 tests/system/runner/harness.py --kernel build/rach2/kernel \
	    --initrd $(METAL_IMAGE) --qemu-arg=-cpu --qemu-arg=max \
	    --net "$(DHCP_NET),hostfwd=tcp::2224-:22" \
	    --qemu-arg=-object \
	    --qemu-arg=filter-dump,id=mt0,netdev=net0,file=$(METAL_PCAP) \
	    --append "rach2.ip=none" \
	    --timeout 180 --steptmo 60 --ok "lease of 192.168.90.20 obtained" \
	    "expect=lease of 192.168.90.20 obtained" wait=8 \
	    "tcp=2224" wait=5
	# The lease line is printed before the script runs, so on its own it proves
	# only that a lease arrived. What proves the machine was configured is the
	# wire: the step above knocks on the SSH port through the address the lease
	# gave it, and the capture must then hold packets sent FROM that address,
	# which only a machine holding it can send. The knock alone proves nothing,
	# because QEMU's user network accepts the host side of a connection before
	# it knows whether the guest is there. Nothing was typed at the console in
	# any of it.
	python3 tests/system/checkers/bcast_check.py $(METAL_PCAP) --source 192.168.90.20
	# And on a network with no server at all, which is a cable plugged in
	# later or a switch that has not started forwarding. The client must go
	# to the background and keep asking rather than give up, because a
	# machine that gave up is one nobody can reach until somebody walks to
	# it, and walking to it is what this milestone exists to avoid. The
	# console login must still be there, so a network that never comes up
	# does not take the local way in with it.
	# And the same on the machine that carries none of busybox: os-simple's
	# own `netup` reads the same `/etc/network/interfaces` and its own `dhcpc`
	# takes the lease. Both images end up on the same network by the same
	# exchange, which is the point of having two clients over one kernel
	# interface.
	#
	# This one runs on a /16, and that is what makes the netmask provable: the
	# classful mask for a 10.x address is /8, so a client that took the address
	# and not the mask would reach 10.200.0.1 by ARPing for it directly. The
	# check below requires that packet to have gone to the gateway instead,
	# which only a machine holding the leased /16 does.
	@rm -f $(SIMPLE_PCAP)
	python3 tests/system/runner/harness.py --kernel build/rach2/kernel \
	    --initrd $(OS_SIMPLE_IMAGE) --qemu-arg=-cpu --qemu-arg=max \
	    --net "$(SIMPLE_NET)" \
	    --qemu-arg=-object \
	    --qemu-arg=filter-dump,id=sp0,netdev=net0,file=$(SIMPLE_PCAP) \
	    --append "rach2.ip=none" \
	    --timeout 240 --steptmo 60 \
	    --ok "lease of 10.90.0.20 netmask 255.255.0.0" \
	    "expect=rach2 login:" "send=root" "expect=Password:" \
	    "send=`cat krffs/rach2_demo_password`" \
	    "expect=The ISH shell" "send=netup -a" \
	    "expect=lease of 10.90.0.20 netmask 255.255.0.0" wait=3 \
	    "send=ping 10.90.0.2" "expect=4/4 replies" \
	    "send=cat /etc/resolv.conf" "expect=nameserver 10.90.0.3" \
	    "send=netup -a" "expect=is already holding it" \
	    "send=ping 10.200.0.1" wait=20 \
	    "send=dhcpc -i eth0 -k" "expect=stopped process" wait=2 \
	    "send=ping 10.90.0.2" "expect=0/4 replies" \
	    "send=netup -a" "expect=lease of 10.90.0.20 netmask 255.255.0.0" wait=3 \
	    "send=ping 10.90.0.2" "expect=4/4 replies"
	# The last four steps are the way back off a lease. A client holds an
	# address for as long as it renews it, so an address is taken off by
	# stopping its client: `dhcpc -k` signals the one that holds the interface
	# and waits for the claim to go. The ping that follows must fail, which is
	# what says the address really came off and not just the process; the
	# `netup -a` after it must take a lease again, which is what says the claim
	# was released; and the ping after that must answer, which is what says the
	# machine is back where it started.
	#
	# Every `expect` above is an assertion, and a missed one fails the run.
	# The success marker is a line only the client prints, because this image
	# runs the ISH shell, which does not take the quotes out of an echoed
	# token the way ash does: a token typed at it would match its own echo.
	python3 tests/system/checkers/bcast_check.py $(SIMPLE_PCAP) --source 10.90.0.20 \
	    --routed-via 10.90.0.2 --routed-dst 10.200.0.1

	# And the other half of the file: a fixed address, on both machines. The
	# format has two methods and only one of them has been exercised above.
	# This is also the sequence an administrator runs to move a machine off
	# DHCP, and every step of it is asserted: the configurator must refuse
	# while a client holds the interface, the client must let go when it is
	# stopped, and the fixed address must then be applied and work. os-simple
	# copies the stanza it stages, since it has no editor; os-real writes one
	# with its shell.
	@rm -f $(STATIC_PCAP)
	python3 tests/system/runner/harness.py --kernel build/rach2/kernel \
	    --initrd $(OS_SIMPLE_IMAGE) --qemu-arg=-cpu --qemu-arg=max \
	    --net "$(SIMPLE_NET)" \
	    --qemu-arg=-object \
	    --qemu-arg=filter-dump,id=st0,netdev=net0,file=$(STATIC_PCAP) \
	    --append "rach2.ip=none" \
	    --timeout 240 --steptmo 60 --ok "4/4 replies" \
	    "expect=rach2 login:" "send=root" "expect=Password:" \
	    "send=`cat krffs/rach2_demo_password`" \
	    "expect=The ISH shell" "send=netup -a" \
	    "expect=lease of 10.90.0.20 netmask 255.255.0.0" wait=3 \
	    "send=cp /etc/network/interfaces.static /etc/network/interfaces" \
	    "send=netup -a" "expect=is holding it with a lease" \
	    "send=dhcpc -i eth0 -k" "expect=stopped process" wait=2 \
	    "send=netup -a" "expect=configured from /etc/network/interfaces" wait=2 \
	    "send=ping 10.90.0.2" "expect=4/4 replies" \
	    "send=ping 10.200.0.1" wait=20
	# The address is the fixed one and not the leased one, which the capture
	# says: packets sent FROM 10.90.0.50, and the off-subnet packet addressed
	# to the gateway's hardware address, which is the route the stanza asked
	# for. Neither is possible from a machine still holding the lease.
	python3 tests/system/checkers/bcast_check.py $(STATIC_PCAP) --source 10.90.0.50 \
	    --routed-via 10.90.0.2 --routed-dst 10.200.0.1
	# The same stanza on os-real, through busybox's `ifup`, which is the whole
	# reason the file has this format: one description of a network, two
	# machines that read it. On the same /16 as the run above, and for the same
	# reason: on a 192.168.x network the mask the stanza asks for is the one the
	# kernel would derive anyway, so skipping the netmask directive entirely
	# would look identical. Here it does not.
	@rm -f $(RSTATIC_PCAP)
	python3 tests/system/runner/harness.py --kernel build/rach2/kernel \
	    --initrd $(OS_REAL_IMAGE) --qemu-arg=-cpu --qemu-arg=max \
	    --net "$(SIMPLE_NET)" \
	    --qemu-arg=-object \
	    --qemu-arg=filter-dump,id=rs0,netdev=net0,file=$(RSTATIC_PCAP) \
	    --append "rach2.ip=none" \
	    --timeout 240 --steptmo 60 --ok "REALSTATICOK" \
	    "expect=rach2 login:" "send=root" "expect=Password:" \
	    "send=`cat krffs/rach2_demo_password`" \
	    "expect=# " \
	    "send=printf 'auto eth0\niface eth0 inet static\naddress 10.90.0.50\nnetmask 255.255.0.0\ngateway 10.90.0.2\n' > /etc/network/interfaces" \
	    "send=ifup -a" wait=5 \
	    "send=ifconfig eth0 > /tmp/if; grep -q 'addr:10.90.0.50' /tmp/if && grep -q 'Mask:255.255.0.0' /tmp/if && ping 10.90.0.2 > /dev/null && echo REALSTATIC\"\"OK" \
	    "expect=REALSTATICOK" "send=ping 10.200.0.1 > /dev/null 2>&1; echo DONE" \
	    "expect=DONE" wait=5
	python3 tests/system/checkers/bcast_check.py $(RSTATIC_PCAP) --source 10.90.0.50 \
	    --routed-via 10.90.0.2 --routed-dst 10.200.0.1

	# And the half of the client that only time reaches: holding the lease.
	# QEMU hands out a day, so `-l` caps what the client will act on, the
	# renewal falls due ten seconds in, and the run must see the lease applied
	# more than once. Without that cap nothing here could reach the renewal
	# path at all, and a client that renewed wrongly would ship green.
	python3 tests/system/runner/harness.py --kernel build/rach2/kernel \
	    --initrd $(OS_SIMPLE_IMAGE) --qemu-arg=-cpu --qemu-arg=max \
	    --net "$(SIMPLE_NET)" --append "rach2.ip=none" \
	    --timeout 180 --steptmo 90 --log $(SIMPLE_LOG) \
	    --ok "lease of 10.90.0.20" \
	    "expect=rach2 login:" "send=root" "expect=Password:" \
	    "send=`cat krffs/rach2_demo_password`" \
	    "expect=The ISH shell" "send=dhcpc -i eth0 -l 20" \
	    "expect=renewed for 20 seconds" wait=30
	# A renewal and a fresh lease are different things, and the difference is
	# what this checks: the client must say it renewed, and it must have asked
	# for an address exactly once. A client that let every lease lapse and
	# discovered again would print the second line as often as the first.
	@r=`grep -ac "renewed for 20 seconds" $(SIMPLE_LOG)`; \
	 a=`grep -ac "asking for an address" $(SIMPLE_LOG)`; \
	    if [ "$$r" -lt 1 ]; then \
	        echo "dhcp-test: the client never renewed its lease" >&2; exit 1; \
	    fi; \
	    if [ "$$a" -ne 1 ]; then \
	        echo "dhcp-test: the client asked for an address $$a times:" \
	             "it was starting over, not renewing" >&2; exit 1; \
	    fi; \
	    echo "dhcp-test: ok: the client renewed $$r time(s) from one exchange"

	# And a boot that was told its address on the command line, on a network
	# with a DHCP server ready to hand out a different one. Init must not run
	# `ifup` there at all: a machine given an address is configured already,
	# and a client would begin by taking that address off and might never put
	# one back. This one is asked at the console rather than read off the
	# wire, because what is being tested is that nothing happened.
	python3 tests/system/runner/harness.py --kernel build/rach2/kernel \
	    --initrd $(METAL_IMAGE) --qemu-arg=-cpu --qemu-arg=max \
	    --net "$(DHCP_NET)" \
	    --append "rach2.ip=192.168.90.50/24 rach2.gateway=192.168.90.2" \
	    --timeout 180 --steptmo 60 --ok "STATICKEPT" \
	    "expect=rach2 login:" "send=root" "expect=Password:" \
	    "send=`cat krffs/rach2_demo_password`" \
	    "expect=# " wait=20 \
	    "send=ifconfig eth0 | grep -q 'addr:192.168.90.50' && echo STATIC\"\"KEPT" \
	    wait=10
	python3 tests/system/runner/harness.py --kernel build/rach2/kernel \
	    --initrd $(METAL_IMAGE) --qemu-arg=-cpu --qemu-arg=max \
	    --net "hubport,id=net0,hubid=1" --append "rach2.ip=none" \
	    --timeout 180 --steptmo 90 --ok "no lease, forking to background" \
	    "expect=rach2 login:" "send=root" "expect=Password:" \
	    "send=`cat krffs/rach2_demo_password`" \
	    "expect=# " "send=echo CONSOLE\"\"UP" \
	    "expect=CONSOLEUP" \
	    "expect=no lease, forking to background" wait=30

# `make netconsole-test`: the kernel log on the wire. The machine this milestone
# targets has no serial port, so the boot log has to leave over Ethernet or not
# at all. QEMU's filter-dump writes every frame the guest sends to a libpcap
# file; the boot below asks for netconsole on the command line, and the checker
# holds the frames to the header's contract and requires a line printed BEFORE
# the network device came up, which is what proves the pre-transmit buffer was
# carried rather than dropped.
NETCONSOLE_PCAP = $(IMAGES_DIR)/netconsole.pcap
run-netconsole: rach2 tests krffs test-image
	@rm -f $(NETCONSOLE_PCAP)
	python3 tests/system/runner/harness.py --kernel build/rach2/kernel \
	    --initrd $(TEST_IMAGE) --qemu-arg=-cpu --qemu-arg=max \
	    --qemu-arg=-object \
	    --qemu-arg=filter-dump,id=nc0,netdev=net0,file=$(NETCONSOLE_PCAP) \
	    --append "rach2.netconsole" \
	    --timeout 90 --ok "Type 'help'" "expect=Type 'help'" wait=3
	python3 tests/system/checkers/netconsole_check.py $(NETCONSOLE_PCAP) --min-frames 4 \
	    --expect "Initializing system components" \
	    --expect "Network... OK" \
	    --expect "netconsole: broadcast"

# `make reset-test`: each reset mechanism on its own. The kernel tries three,
# most of the machine first, and the order was chosen for a laptop whose
# chipset register may not act; the risk in that choice is that only the first
# one is ever exercised, and nobody finds out the later ones do not work until
# the machine they were written for refuses to restart. So each is booted with
# the ones before it left out, and each must still restart the machine.
run-reset: rach2 tests krffs test-image
	sh tests/system/suites/reset.sh build/rach2/kernel $(TEST_IMAGE)

# `make diskdiag-test`: a disk read without being mounted and without being
# written. Three boots of one freshly built image: the ordinary kernel mounting
# it, which must change it (otherwise the comparisons below prove nothing); the
# read-only kernel reading it over SSH with nothing typed at the console, where
# every sector hashed on the machine must equal the same offset hashed here;
# and the same kernel with no drive at all.
run-diskdiag: rach2 rach2-metal rach2-metal-image
	sh tests/system/suites/diskdiag.sh build/rach2/kernel $(METAL_KERNEL) $(METAL_IMAGE) 2224

# `make subset-test`: the kernel reaches a shell with any subset of its optional
# devices switched off. On a machine with no serial port, a boot that stops has
# no diagnostic but the screen, and the only way to find which probe stopped it
# is to switch them off one at a time from the GRUB command line. That is only
# useful if each switch actually works, so each is booted here.
run-subset: rach2 tests krffs test-image
	@for opt in "" "rach2.noaudio" "rach2.nomouse" "rach2.nonet" "rach2.nodisk" \
	            "rach2.noaudio rach2.nomouse rach2.nonet rach2.nodisk"; do \
	    echo "  subset-test: booting with [$$opt]"; \
	    python3 tests/system/runner/harness.py --kernel build/rach2/kernel \
	        --initrd $(TEST_IMAGE) --qemu-arg=-cpu --qemu-arg=max \
	        --append "$$opt" --timeout 90 --ok "Type 'help'" \
	        "expect=Type 'help'" wait=2 >/dev/null 2>&1 \
	        || { echo "subset-test: FAIL: no shell with [$$opt]" >&2; exit 1; }; \
	done
	@echo "subset-test: PASS"

# `make diskpolicy-test`: the read-only kernel does not write the disk, shown on
# the bytes rather than on a return value. The same disk is booted with each
# kernel; the ordinary one must change it, which is what makes the other half
# mean anything, and the metal one must refuse a command by name and leave the
# image untouched.
run-diskpolicy: rach2 rach2-metal
	sh tests/system/suites/diskpolicy.sh build/rach2/kernel $(METAL_KERNEL)

# `make metal-test`: the metal image reached over SSH with nobody at its
# console. This is the rehearsal for the Lenovo: the machine is given power and
# an address on its command line, and nothing is typed at it. What that proves
# is everything that has to happen by itself for a machine across the room to be
# usable, so the address configuration, init-metal's supervision of dropbear,
# key authentication, a pty, and a file crossing intact each way are all one
# test. It kills the server once as well, because a supervisor that starts a
# daemon and does not restart it has not been tested.
run-metal: rach2-metal rach2-metal-image
	sh tests/system/suites/metal.sh $(METAL_KERNEL) $(METAL_IMAGE) 2223

# `make account-test`: accounts created, authenticated, changed and deleted on
# both images, since both have account tools now and they are different code.
# os-real gets busybox's over three boots, so what is asserted is what reached
# the platter and not what one process had in its cache; os-simple gets this
# project's own in RAM, where there is no `deluser` to finish with. Both
# authenticate from an unprivileged account, since root's `su` asks for no
# password and would pass over a wrong hash and an unusable set-id `passwd`
# alike.
ACCOUNT_TEST_DISK = $(IMAGES_DIR)/account-test.disk

run-account: rach2 utils krffs busybox ish check-tcc check-pdpmake
	sh krffs/gen_demo_key.sh
	sh krffs/gen_demo_password.sh
	$(MKDISK) krffs/manifests/os-real.manifest $(ACCOUNT_TEST_DISK) 64 48
	sh tests/system/suites/account.sh build/rach2/kernel $(ACCOUNT_TEST_DISK) \
	    "`cat krffs/rach2_demo_password`" real
	$(MKIMAGE) krffs/manifests/os-simple.manifest $(OS_SIMPLE_IMAGE) 10
	sh tests/system/suites/account.sh build/rach2/kernel $(OS_SIMPLE_IMAGE) \
	    "`cat krffs/rach2_demo_password`" simple

# `make tcc-test`: the machine compiling a program on itself. Boots os-real from
# a disk as the unprivileged `test` account, writes a C file at the prompt,
# compiles it, runs the result, and then asks for the four kinds of program this
# system cannot run and requires each to be refused with its reason and to leave
# no output behind. The boot carries `rach2.trace=enosys`, so a syscall the
# compiler wants and this kernel lacks is named on the console rather than
# turning into a mysterious failure.
TCC_TEST_DISK = $(IMAGES_DIR)/tcc-test.disk

run-tcc: rach2 utils krffs check-busybox check-tcc check-pdpmake check-setid check-dropbearmulti ports/doom
	sh krffs/gen_demo_key.sh
	sh krffs/gen_demo_password.sh
	$(MKDISK) krffs/manifests/os-real.manifest $(TCC_TEST_DISK) 64 48
	sh tests/system/suites/tcc.sh build/rach2/kernel $(TCC_TEST_DISK) \
	    "`cat krffs/rach2_demo_password`" "`cat krffs/rach2_test_password`"

# `make make-test`: the machine building a program on itself, and the claim a
# compiler alone cannot make: that a second build does nothing, that touching
# one source rebuilds one object, and that the answer survives a reboot. Two
# boots of one disk, since the reboot is half the point.
MAKE_TEST_DISK = $(IMAGES_DIR)/make-test.disk

run-make: rach2 utils krffs check-busybox check-tcc check-pdpmake check-setid check-dropbearmulti ports/doom
	@mkdir -p $(IMAGES_DIR)
	sh krffs/gen_demo_password.sh
	$(MKDISK) krffs/manifests/os-real.manifest $(MAKE_TEST_DISK) 64 48
	sh tests/system/suites/make.sh build/rach2/kernel $(MAKE_TEST_DISK) \
	    "`cat krffs/rach2_test_password`"

# `make selfbuild-test`: the machine builds its own kernel. Boots os-real from
# a disk copy as the unprivileged test account, copies /src/rach2 home, and
# runs the staged make and compiler over the staged guest.mk, twice from clean
# workspaces; the two kernels must hash equal. Every wait is bounded, and the
# whole run carries one deadline over the per-step ones.
SELFBUILD_DISK = $(IMAGES_DIR)/selfbuild-test.disk

run-selfbuild: rach2 utils krffs test-image check-busybox check-tcc check-pdpmake check-setid check-dropbearmulti ports/doom
	@mkdir -p $(IMAGES_DIR)
	sh krffs/gen_demo_password.sh
	$(MKDISK) krffs/manifests/os-real.manifest $(SELFBUILD_DISK) 64 48
	sh tests/system/suites/selfbuild.sh build/rach2/kernel $(SELFBUILD_DISK) \
	    "`cat krffs/rach2_test_password`" "`cat krffs/rach2_demo_password`"

# `make ssh-test`: the SSH capstone with nobody at the keyboard. Boots os-real
# from a disk with the port forwarded, starts dropbear from the console session
# the way the Readme's recipe does, then logs in twice: root by key, and the
# unprivileged account by password, which is the half that reaches /etc/shadow.
# Every wait is bounded, so a hang fails instead of sitting there.
SSH_TEST_DISK = $(IMAGES_DIR)/ssh-test.disk
SSH_TEST_PORT ?= 2222

run-ssh: rach2 utils krffs check-busybox check-tcc check-pdpmake check-setid check-dropbearmulti ports/doom
	sh krffs/gen_demo_key.sh
	sh krffs/gen_demo_password.sh
	$(MKDISK) krffs/manifests/os-real.manifest $(SSH_TEST_DISK) 64 48
	sh tests/system/suites/ssh.sh build/rach2/kernel $(SSH_TEST_DISK) \
	    "`cat krffs/rach2_demo_password`" "`cat krffs/rach2_test_password`" \
	    $(SSH_TEST_PORT)

# `make shutdown-test`: userland stopping the machine, on a disk fixture, with
# every claim of a stop asserted on the QEMU process ending rather than on a
# token, and every image checked by offline fsck afterwards.
SHUTDOWN_TEST_DISK = $(IMAGES_DIR)/shutdown-test.disk

run-shutdown: rach2 utils krffs busybox
	sh krffs/gen_demo_password.sh
	$(MKDISK) krffs/manifests/shutdown-test.manifest $(SHUTDOWN_TEST_DISK) 64 48
	sh tests/system/suites/shutdown.sh build/rach2/kernel $(SHUTDOWN_TEST_DISK) \
	    build/krffs/fsck.krffs "`cat krffs/rach2_demo_password`"

# `make login-test`: prove the console cannot fall back to an unauthenticated
# shell. Two images differing only in the init they declare, neither staging
# /bin/login, plus a rescue boot of the real image, which is the way back into
# one that will not offer a prompt.
LOGIN_TEST_STRICT = $(IMAGES_DIR)/login-test-strict.krffs
LOGIN_TEST_SHELL  = $(IMAGES_DIR)/login-test-shell.krffs

run-login: rach2 utils krffs busybox os-real-image
	$(MKIMAGE) krffs/manifests/login-test-strict.manifest $(LOGIN_TEST_STRICT) 10
	$(MKIMAGE) krffs/manifests/login-test-shell.manifest $(LOGIN_TEST_SHELL) 10
	sh tests/system/suites/login.sh build/rach2/kernel $(LOGIN_TEST_STRICT) \
	    $(LOGIN_TEST_SHELL) $(OS_REAL_IMAGE) root "`cat krffs/rach2_demo_password`"

# `make procfiles-test`: the machine's own /proc files, on the boots that differ
# and through the programs that read them. Three boots of the test image, one
# per mount source, each of which must name its own; then `ps`, `df`, `free` and
# `uptime` on the real image, each of which failed on a missing /proc file.
PROCFILES_DISK = $(IMAGES_DIR)/procfiles-test.disk
run-procfiles: rach2 tests utils krffs busybox os-real-image test-image
	$(MKDISK) krffs/manifests/disk-test.manifest $(PROCFILES_DISK) 64 48
	sh tests/system/suites/procfiles.sh build/rach2/kernel $(TEST_IMAGE) \
	    $(PROCFILES_DISK) $(OS_REAL_IMAGE) root "`cat krffs/rach2_demo_password`" 16

# `make exitline-test`: the plain process-exit line is printed under the debug
# shell and nowhere else. Four boots: an init image that must stay quiet while
# a forked program runs, the test image that must still print it for a child
# and a probe, a killed probe whose dedicated marker must survive, and a
# harness.py run with no token, which has only the plain line to read.
HARNESS_KERNEL = build/rach2-harness/kernel

run-exitline: rach2 tests utils krffs busybox os-real-image test-image
	$(MAKE) -C rach2 COMPONENT=rach2-harness HARNESS=1
	sh tests/system/suites/exitline.sh build/rach2/kernel $(TEST_IMAGE) \
	    $(OS_REAL_IMAGE) root "`cat krffs/rach2_demo_password`" \
	    $(HARNESS_KERNEL)

run-crash: rach2 tests krffs
	$(MKDISK) krffs/manifests/disk-test.manifest $(CRASH_TEST_IMAGE) 64 48
	sh tests/system/suites/crash.sh build/rach2/kernel $(CRASH_TEST_IMAGE) build/krffs/fsck.krffs

# `make stress-test`: prove an on-disk KRFFS larger than physical RAM works. Boots
# a 64 MB disk with a 48 MB partition and runs stressprobe, which writes and
# re-reads 33 MiB (past the 32 MiB RAM cap) through the 4 MiB cache.
STRESS_DISK = $(IMAGES_DIR)/stress-test.disk
run-stress: rach2 tests krffs
	$(MKDISK) krffs/manifests/stress-test.manifest $(STRESS_DISK) 64 48
	python3 tests/system/runner/harness.py --kernel build/rach2/kernel \
	    --append "rach2.fsdisk rach2.rescue" \
	    --drive "file=$(STRESS_DISK),format=raw,if=ide,index=0,media=disk" \
	    --timeout 150 --ok "STRESSPROBE OK" \
	    "expect=Type 'help'" "send=exec /stressprobe" wait=130

# The AHCI variants of the disk tests: identical images and probes, but the disk
# rides an ich9-ahci HBA (if=none + ide-hd) and rach2.disk=ahci selects the AHCI
# backend, exercising the same page-cache/KRFFS path through ahci_read/ahci_write.
# The --qemu-arg passthrough adds the two -device lines without disturbing the
# fixed NIC. The ATA test-disk/persist-test/stress-test stay on if=ide.
AHCI_ATTACH = --qemu-arg=-device --qemu-arg=ich9-ahci,id=ahci0 \
              --qemu-arg=-device --qemu-arg=ide-hd,drive=drv0,bus=ahci0.0

# Per-bus image names so an ATA target and its AHCI twin can run under `make -j`
# without racing on a shared file (each recipe re-stages its image first).
DISK_TEST_AHCI_IMAGE = $(IMAGES_DIR)/disk-test-ahci.disk
PERSIST_AHCI_DISK    = $(IMAGES_DIR)/persist-test-ahci.disk
STRESS_AHCI_DISK     = $(IMAGES_DIR)/stress-test-ahci.disk

run-disk-ahci: rach2 tests krffs
	$(MKDISK) krffs/manifests/disk-test.manifest $(DISK_TEST_AHCI_IMAGE) 64 48
	python3 tests/system/runner/harness.py --kernel build/rach2/kernel \
	    --append "rach2.disk=ahci rach2.fsdisk rach2.rescue" \
	    --drive "file=$(DISK_TEST_AHCI_IMAGE),format=raw,if=none,id=drv0" \
	    $(AHCI_ATTACH) \
	    --timeout 90 --ok "BLOCKPROBE OK" probe=blockprobe

# `make test-disk-q35`: the same disk proof on the q35 machine, whose AHCI
# controller is built in at 0:31.2, function 2 of the multi-function device 31.
# That is the laptop's exact topology, and no other test exercises it: every
# other AHCI test attaches a separate controller that lands at function 0, so a
# scan that never walked past function 0 would pass all of them and fail on
# the real machine.
run-disk-q35: rach2 tests krffs
	$(MKDISK) krffs/manifests/disk-test.manifest $(DISK_TEST_AHCI_IMAGE) 64 48
	python3 tests/system/runner/harness.py --kernel build/rach2/kernel \
	    --append "rach2.disk=ahci rach2.fsdisk rach2.rescue" \
	    --drive "file=$(DISK_TEST_AHCI_IMAGE),format=raw,if=none,id=drv0" \
	    --qemu-arg=-machine --qemu-arg=q35 --qemu-arg=-cpu --qemu-arg=max \
	    --qemu-arg=-device --qemu-arg=ide-hd,drive=drv0,bus=ide.0 \
	    --timeout 90 --ok "BLOCKPROBE OK" probe=blockprobe

run-persist-ahci: rach2 tests krffs
	$(MKDISK) krffs/manifests/persist-test.manifest $(PERSIST_AHCI_DISK) 64 48
	python3 tests/system/runner/harness.py --kernel build/rach2/kernel \
	    --append "rach2.disk=ahci rach2.fsdisk rach2.rescue" \
	    --drive "file=$(PERSIST_AHCI_DISK),format=raw,if=none,id=drv0" \
	    $(AHCI_ATTACH) \
	    --timeout 60 --ok "PERSISTPROBE OK (wrote)" probe=persistprobe
	python3 tests/system/runner/harness.py --kernel build/rach2/kernel \
	    --append "rach2.disk=ahci rach2.fsdisk rach2.rescue" \
	    --drive "file=$(PERSIST_AHCI_DISK),format=raw,if=none,id=drv0" \
	    $(AHCI_ATTACH) \
	    --timeout 60 --ok "PERSISTPROBE OK (read back)" probe=persistprobe

run-stress-ahci: rach2 tests krffs
	$(MKDISK) krffs/manifests/stress-test.manifest $(STRESS_AHCI_DISK) 64 48
	python3 tests/system/runner/harness.py --kernel build/rach2/kernel \
	    --append "rach2.disk=ahci rach2.fsdisk rach2.rescue" \
	    --drive "file=$(STRESS_AHCI_DISK),format=raw,if=none,id=drv0" \
	    $(AHCI_ATTACH) \
	    --timeout 150 --ok "STRESSPROBE OK" \
	    "expect=Type 'help'" "send=exec /stressprobe" wait=130

# Bootable ISO images for real hardware: each embeds the kernel and one RAM-disk
# image as GRUB multiboot modules, built by grub-mkrescue in a throwaway Docker
# container (see tools/image/mkiso.sh). Output: build/images/<image>.iso.
isos: test-iso os-simple-iso os-real-iso rach2-metal-iso
test-iso: test-image
	sh tools/image/mkiso.sh test
os-simple-iso: os-simple-image
	sh tools/image/mkiso.sh os-simple
os-real-iso: os-real-image
	sh tools/image/mkiso.sh os-real
rach2-metal-iso: rach2-metal rach2-metal-image
	sh tools/image/mkiso.sh rach2-metal

# The staged capstone binaries are the REAL prerequisites of the os-real image. A
# stamp only proves its build RAN; the artifact can be deleted OR truncated afterwards
# while the stamp lingers, so these PHONY check targets (depending on the stamp so it
# builds first) run `test -s` on EVERY build and catch a missing OR zero-byte artifact.
# Both binaries, both applet tables and both resolved configs, because a stamp
# file outlives an artifact somebody deleted or truncated, and the set-id ones
# are the artifacts whose absence would be noticed last and matter most.
# The compiler the guest runs, and the two things it cannot work without: its
# runtime and its own headers. The ELF check is the one that matters: `file(1)`
# would call a Mach-O or an arm64 build "an executable" and this kernel would
# refuse it, and a dynamically linked one it would ACCEPT and then run
# unrelocated, which is worse. The last check reads the output policy's own
# diagnostics out of the shipped binary, since a patch that did not apply and a
# compiler that accepts `-run` look identical from outside.
check-pdpmake: $(PDPMAKE_STAMP) $(PDPMAKE_BIN)
	@test -s $(PDPMAKE_BIN) || { \
	    echo "pdpmake: $(PDPMAKE_BIN) missing or empty; rm vendor/pdpmake/.stamp and rebuild (make pdpmake)" >&2; \
	    exit 1; }
	@python3 tools/inspect/check_elf.py $(PDPMAKE_BIN)

check-tcc: $(TCC_STAMP) $(TCC_OUTPUTS) $(BINFIND)
	@for f in $(TCC_BIN) build/vendor/tcc/libtcc1.a \
	          build/vendor/tcc/include/stdarg.h \
	          build/vendor/tcc/include/stddef.h \
	          build/vendor/tcc/include/stdbool.h \
	          build/vendor/tcc/include/float.h \
	          build/vendor/tcc/include/varargs.h; do \
	    test -s "$$f" || { echo "tcc: $$f missing or empty; rm vendor/tcc/.stamp and rebuild (make tcc)" >&2; exit 1; }; \
	done
	@python3 tools/inspect/check_elf.py --selftest
	@python3 tools/inspect/check_elf.py $(TCC_BIN)
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

# Privilege climbs through a declared set of paths and no others. The sweep reads
# every manifest against a per-image allowlist, so a set-id line anywhere, under any
# of the five mode-bearing directives, fails the build rather than a boot. Every
# artifact a check reads becomes a prerequisite here, in the phase that adds the
# check, because make does not order the siblings of a prerequisite list.
# The manifest sweep needs no build product, but the applet-table check reads
# one, so this target is ordered against the busybox stamp. Unordered it would
# fail a correct tree on a header that does not exist yet, or, in the case that
# matters, read the PREVIOUS configuration's table after rach2.config was edited,
# pass, and let the image stage the busybox whose re-enabled applet the check
# exists to forbid: build_busybox.sh opens by removing and re-extracting the
# tree, so for most of a rebuild the header is absent and before that it is
# stale.
# The applet half reads a generated header and the init half reads two built
# binaries, so both are ordered after what produces them, the way phase 3's rule
# requires: unordered, a parallel build scans a build/utils/sbin/ that holds
# yesterday's artifacts and passes while today's are staged.
check-setid: $(BUSYBOX_STAMP) utils
	@python3 tools/inspect/check_setid.py
	@python3 tools/inspect/check_setid.py --applets
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

# --- toolchain preflight + quality gates ---
doctor:
	@echo "doctor: retired: make checks its own tools; this remains the setup report" >&2
	@./tools/setup/doctor.sh

# The style suite (see tools/verify/check.sh): -Werror build, scoped cppcheck,
# shellcheck, the convention grep, clang-format, and a readelf ABI check.
# `tests/run style` names this target.
run-style:
	@./tools/verify/check.sh

# The host suite (see tests/system/suites/host.sh): pure kernel logic compiled and run
# natively under AddressSanitizer + UndefinedBehaviorSanitizer. It waits on
# the vendored musl for the same reason krffs does: mkhash_hosttest names the
# musl crypt in its host-deps, and that source exists only once the tarball is
# extracted. `tests/run host` names this target.
run-host: check-libc $(DEADLINE)
	@./tests/system/suites/host.sh

# Retired names, kept as aliases for this milestone only.
verify-style:
	@echo "verify-style: retired: use tests/run style" >&2
	@./tests/run style

verify-host:
	@echo "verify-host: retired: use tests/run host" >&2
	@./tests/run host

# --- IDE support + source-list guard ---
# clangd compilation database, generated from the build's own commands (GCC has no
# clang-style -MJ). Output: compile_commands.json at the repo root (git-ignored). The
# checked-in .clangd points clangd at the i686-elf cross GCC; see mk/Readme.md.
# Retired name, kept as an alias for this milestone only.
compile-commands:
	@echo "compile-commands: retired: run sh tools/dev/gen_compile_commands.sh" >&2
	@./tools/dev/gen_compile_commands.sh

# Kernel source-list guard: fail if a rach2 .c on disk is missing from C_SRC (so a new
# kernel source cannot be silently dropped from the build). Also run inside the gate.
check-sources:
	@./tools/verify/check_sources.sh

# clean: the everyday reset. Each sub-make removes its own build/<component> tree
# (objects, .d, binaries), and the RAM-disk images under build/images go too. The
# expensive vendored third-party is LEFT in place: vendor/musl/install, the extracted
# busybox/dropbear trees, and the staged capstones under build/vendor (which no
# sub-make owns). Use distclean for those.
clean:
	@for dir in $(BASE_DIRS) $(FULL_DIRS); do $(MAKE) -C $$dir clean; done
	rm -rf build/images build/iso

# distclean: a full from-scratch reset; clean plus every third-party build product: the
# stamps, vendor/musl/install, the extracted vendored source trees, and the entire
# out-of-tree build/ (including the staged capstones under build/vendor that a routine
# clean leaves in place). The next build rebuilds musl/busybox/dropbear/tcc/pdpmake
# from the tarballs. A stamp left behind while its binary went with build/ is the
# failure this list exists to prevent: the next build would skip the work and
# fail at the check instead.
distclean: clean
	rm -f $(LIBC_STAMP) $(BUSYBOX_STAMP) $(DROPBEAR_STAMP) $(TCC_STAMP) \
	      $(PDPMAKE_STAMP) vendor/musl/.outputs vendor/tcc/.outputs \
	      vendor/tcc/.validated vendor/pdpmake/.validated
	rm -rf vendor/musl/install \
	       $(LIBC_TARBALL:.tar.gz=) $(BUSYBOX_TARBALL:.tar.bz2=) \
	       $(DROPBEAR_TARBALL:.tar.bz2=) $(TCC_TARBALL:.tar.bz2=) \
	       $(PDPMAKE_TARBALL:.tgz=)
	rm -rf build

help:
	@echo "help: retired: the Readme documents the interface" >&2
	@echo "  make            build everything (kernel, userland, the three RAM images)"
	@echo "  make test       build what the default suite needs and run it"
	@echo "  tests/run NAME  run one suite (style, host, ssh, all, ...)"
	@echo "  make clean      remove build output; keep the vendor builds and credentials"
	@echo "  make distclean  remove the vendor builds and stamps too"
	@echo "  rach2/run.sh IMAGE   boot a built image (test, os-simple, os-real)"

# Retired suite names, kept as aliases for this milestone only.
test-addressed:
	@echo "test-addressed: retired: use tests/run addressed" >&2
	@./tests/run addressed
test-noentropy:
	@echo "test-noentropy: retired: use tests/run noentropy" >&2
	@./tests/run noentropy
test-disk:
	@echo "test-disk: retired: use tests/run disk" >&2
	@./tests/run disk
test-disk-ahci:
	@echo "test-disk-ahci: retired: use tests/run disk-ahci" >&2
	@./tests/run disk-ahci
test-disk-q35:
	@echo "test-disk-q35: retired: use tests/run disk-q35" >&2
	@./tests/run disk-q35
bootpart-test:
	@echo "bootpart-test: retired: use tests/run bootpart" >&2
	@./tests/run bootpart
subset-test:
	@echo "subset-test: retired: use tests/run subset" >&2
	@./tests/run subset
stress-test:
	@echo "stress-test: retired: use tests/run stress" >&2
	@./tests/run stress
stress-test-ahci:
	@echo "stress-test-ahci: retired: use tests/run stress-ahci" >&2
	@./tests/run stress-ahci
persist-test:
	@echo "persist-test: retired: use tests/run persist" >&2
	@./tests/run persist
persist-test-ahci:
	@echo "persist-test-ahci: retired: use tests/run persist-ahci" >&2
	@./tests/run persist-ahci
cred-persist-test:
	@echo "cred-persist-test: retired: use tests/run cred-persist" >&2
	@./tests/run cred-persist
crash-test:
	@echo "crash-test: retired: use tests/run crash" >&2
	@./tests/run crash
authority-test:
	@echo "authority-test: retired: use tests/run authority" >&2
	@./tests/run authority
bootline-test:
	@echo "bootline-test: retired: use tests/run bootline" >&2
	@./tests/run bootline
exitline-test:
	@echo "exitline-test: retired: use tests/run exitline" >&2
	@./tests/run exitline
login-test:
	@echo "login-test: retired: use tests/run login" >&2
	@./tests/run login
procfiles-test:
	@echo "procfiles-test: retired: use tests/run procfiles" >&2
	@./tests/run procfiles
account-test:
	@echo "account-test: retired: use tests/run account" >&2
	@./tests/run account
shutdown-test:
	@echo "shutdown-test: retired: use tests/run shutdown" >&2
	@./tests/run shutdown
reset-test:
	@echo "reset-test: retired: use tests/run reset" >&2
	@./tests/run reset
diskpolicy-test:
	@echo "diskpolicy-test: retired: use tests/run diskpolicy" >&2
	@./tests/run diskpolicy
diskdiag-test:
	@echo "diskdiag-test: retired: use tests/run diskdiag" >&2
	@./tests/run diskdiag
ping-test:
	@echo "ping-test: retired: use tests/run ping" >&2
	@./tests/run ping
packet-test:
	@echo "packet-test: retired: use tests/run packet" >&2
	@./tests/run packet
raw-test:
	@echo "raw-test: retired: use tests/run raw" >&2
	@./tests/run raw
netif-test:
	@echo "netif-test: retired: use tests/run netif" >&2
	@./tests/run netif
dgram-test:
	@echo "dgram-test: retired: use tests/run dgram" >&2
	@./tests/run dgram
dhcp-test:
	@echo "dhcp-test: retired: use tests/run dhcp" >&2
	@./tests/run dhcp
netconsole-test:
	@echo "netconsole-test: retired: use tests/run netconsole" >&2
	@./tests/run netconsole
tcc-test:
	@echo "tcc-test: retired: use tests/run tcc" >&2
	@./tests/run tcc
make-test:
	@echo "make-test: retired: use tests/run make" >&2
	@./tests/run make
selfbuild-test:
	@echo "selfbuild-test: retired: use tests/run selfbuild" >&2
	@./tests/run selfbuild
ssh-test:
	@echo "ssh-test: retired: use tests/run ssh" >&2
	@./tests/run ssh
grub-test:
	@echo "grub-test: retired: use tests/run grub" >&2
	@./tests/run grub
menu-test:
	@echo "menu-test: retired: use tests/run menu" >&2
	@./tests/run menu
install-test:
	@echo "install-test: retired: use tests/run install" >&2
	@./tests/run install
sealed-test:
	@echo "sealed-test: retired: use tests/run sealed" >&2
	@./tests/run sealed
drill-test:
	@echo "drill-test: retired: use tests/run drill" >&2
	@./tests/run drill
metal-test:
	@echo "metal-test: retired: use tests/run metal" >&2
	@./tests/run metal

.PHONY: run-addressed run-noentropy run-disk run-disk-ahci run-disk-q35 run-bootpart run-subset run-stress run-stress-ahci run-persist run-persist-ahci run-cred-persist run-crash run-authority run-bootline run-exitline run-login run-procfiles run-account run-shutdown run-reset run-diskpolicy run-diskdiag run-ping run-packet run-raw run-netif run-dgram run-dhcp run-netconsole run-tcc run-make run-selfbuild run-ssh run-grub run-menu run-install run-sealed run-drill run-metal
