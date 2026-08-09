# Rach2 root Makefile.
#
# The build produces purpose-built RAM-disk images, each its own file under
# build/images/ so they coexist: `test` (the automated end-to-end test binaries),
# `os-simple` (our own shell and simple Unix tools, for teaching), and `os-real`
# (a real Unix-like environment: busybox, dropbear, Doom).
#
# Quick start (run `make doctor` first to check your toolchain):
#   make            build the kernel + the os-simple image: the everyday teaching
#                   build, kept fast.
#   make full       all the RAM-disk images (test, os-simple, os-real).
#   make images     the RAM-disk images.
#   make test       boot the test image and check every probe passes.
#   make isos       a bootable ISO per image, for real hardware (needs Docker).
#   make deps       build the vendored third-party (libc; DEPS=full adds bb+dropbear)
#   make doctor     check the toolchain (PASS/MISSING with install hints)
#   make verify-style / verify-host   the quality gates
#   make clean      remove the in-house build products and the images
#   make distclean  + the vendored musl/busybox/dropbear builds (full from-scratch)
#   make help       list every target
# Run a built image with rach2/run.sh <test|os-simple|os-real>; the build system
# does not run QEMU (that script models the disk/audio/network/-cpu matrix).

# --- components (each builds via its own sub-make) ---
BASE_DIRS = krffs rach2 tests ish utils
FULL_DIRS = ports/doom

# --- vendored third-party: built by their own scripts (which wrap musl's autotools
#     and busybox/dropbear's kbuild), tracked with stamp files so a clean clone
#     "just works" without running the build_*.sh by hand, and each rebuilds only
#     when its real inputs (tarball, build script, config) change. ---
LIBC_STAMP     = vendor/musl/.stamp
BUSYBOX_STAMP  = vendor/busybox/.stamp
DROPBEAR_STAMP = vendor/dropbear/.stamp

# The staged capstone binaries the build_*.sh scripts produce, out of tree under
# build/vendor/ (mirroring their vendor/<name>/ sources). A routine `make clean` never
# reaches build/vendor, so these expensive artifacts survive it; only `make distclean`
# removes them. They are the REAL prerequisites of the capstone image (see the check
# targets below): a stamp only proves its build RAN.
BUSYBOX_BIN  = build/vendor/busybox/busybox
DROPBEAR_BIN = build/vendor/dropbear/dropbearmulti

# The pinned vendored source tarballs: the exact names the build_*.sh scripts
# verify by sha256. Used as stamp prerequisites (not a $(wildcard), which would
# silently drop the prereq and look up-to-date if a tarball were deleted) and to
# derive the extracted source dirs that `distclean` removes.
LIBC_TARBALL     = vendor/musl/musl-1.2.5.tar.gz
BUSYBOX_TARBALL  = vendor/busybox/busybox-1.36.1.tar.bz2
DROPBEAR_TARBALL = vendor/dropbear/dropbear-2022.83.tar.bz2

# The two musl artifacts consumers depend on, both derived from the tarball name so
# the pinned version is written once: the installed static libc the cross-compiled
# userland links against, and the extracted source, which the host-compiled mkhash
# and its seam test read the SHA-512 crypt from.
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
OS_REAL_DISK    = $(IMAGES_DIR)/os-real.disk
MKIMAGE         = krffs/mkimage.sh
MKDISK          = krffs/mkdisk.sh

.PHONY: all full deps clean distclean images musl-programs test test-disk \
        test-image os-simple-image os-real-image os-real-disk persist-test \
        crash-test stress-test os-real-grub-disk \
        test-disk-ahci persist-test-ahci stress-test-ahci \
        isos test-iso os-simple-iso os-real-iso \
        check-busybox check-busybox-suid check-dropbearmulti check-libc \
        compile-commands check-sources \
        verify-style verify-host doctor help \
        busybox dropbear $(BASE_DIRS) $(FULL_DIRS)

# Default: the kernel and the os-simple image (our shell + simple Unix tools).
all: os-simple-image

# Everything: all the RAM-disk images. Each is its own file under build/images/, so
# they coexist instead of overwriting one shared image.
full: images

# Build the vendored third-party. libc is built by default (utils link it); busybox
# and dropbear are added with DEPS=full (they are slow capstones).
deps: $(LIBC_STAMP)
ifeq ($(DEPS),full)
deps: $(BUSYBOX_STAMP) $(DROPBEAR_STAMP)
endif

# Friendly aliases for the third-party stamps.
busybox: $(BUSYBOX_STAMP)
dropbear: $(DROPBEAR_STAMP)

# --- third-party stamp rules. Each removes BOTH the stamp and the expected artifact
#     up front, runs its build script, requires a non-empty artifact, then touches
#     the stamp. Removing the artifact first means a script that exits 0 without
#     restaging cannot pass the assertion on a STALE artifact; removing the stamp
#     first means any failure (build or assertion) leaves no stamp, so the next make
#     retries instead of trusting a missing/old artifact. .DELETE_ON_ERROR is the
#     belt-and-suspenders backstop on the final touch. ---
.DELETE_ON_ERROR:

$(LIBC_STAMP): vendor/musl/build_libc.sh $(LIBC_TARBALL)
	@rm -f $@ vendor/musl/install/lib/libc.a
	./vendor/musl/build_libc.sh
	@test -s vendor/musl/install/lib/libc.a || { echo "libc: build produced no libc.a" >&2; exit 1; }
	@touch $@

$(BUSYBOX_STAMP): vendor/busybox/build_busybox.sh vendor/busybox/rach2.config vendor/busybox/muslgcc \
                  $(BUSYBOX_TARBALL) $(LIBC_STAMP)
	@rm -f $@ $(BUSYBOX_BIN)
	./vendor/busybox/build_busybox.sh
	@test -s $(BUSYBOX_BIN) || { echo "busybox: build produced no $(BUSYBOX_BIN)" >&2; exit 1; }
	@touch $@

$(DROPBEAR_STAMP): vendor/dropbear/build_dropbear.sh vendor/dropbear/localoptions.h vendor/dropbear/muslgcc \
                   $(DROPBEAR_TARBALL) $(LIBC_STAMP)
	@rm -f $@ $(DROPBEAR_BIN)
	./vendor/dropbear/build_dropbear.sh
	@test -s $(DROPBEAR_BIN) || { echo "dropbear: build produced no $(DROPBEAR_BIN)" >&2; exit 1; }
	@touch $@

# --- component sub-makes + the build-order DAG ---
$(BASE_DIRS) $(FULL_DIRS):
	$(MAKE) -C $@

# krffs waits on the vendored musl because mkhash compiles its SHA-512 crypt from
# that source, which exists only once build_libc.sh has extracted the pinned
# tarball; without this a from-scratch tree (a fresh clone, or make distclean)
# builds krffs first and cannot find the file.
krffs: check-libc
rach2: krffs
tests: rach2 $(LIBC_STAMP)
ish: rach2
utils: rach2 $(LIBC_STAMP)
ports/doom: rach2

# --- the RAM-disk images. Each target builds its inputs (via the component
#     sub-makes), then stages the image from its manifest with mkimage.sh
#     (dd + mkfs.krffs + stageimage.krffs), out of tree into build/images/. The
#     image targets are .PHONY: a re-stage is sub-second, so they always refresh. ---

images: test-image os-simple-image os-real-image

# The musl integration programs (tests/musl/*.c -> build/tests/musl/), built by
# their own script. They are end-to-end libc tests staged into the test image.
musl-programs: $(LIBC_STAMP)
	sh tests/musl/build.sh

# The test image: the freestanding syscall probes plus the musl integration
# programs. `make test` boots it and asserts every probe's success token.
test-image: krffs rach2 tests musl-programs utils
	$(MKIMAGE) krffs/manifests/test.manifest $(TEST_IMAGE) 10

# The os-simple image: /sbin/init (which forks the ish shell) and the hand-written utils.
os-simple-image: krffs rach2 ish utils
	$(MKIMAGE) krffs/manifests/os-simple.manifest $(OS_SIMPLE_IMAGE) 10

# The os-real image: a real Unix-like environment (busybox shell + coreutils,
# dropbear SSH server and client, Doom, and an /etc + /root tree). 16 MB: enough for
# the capstones and the shareware Doom WAD, and it still fits under the kernel's
# 32 MB physical-RAM cap once the kernel and its frames are accounted for (the module
# is loaded just above 1 MB). gen_demo_key.sh provisions the SSH login keys and
# gen_demo_password.sh the account passwords and /etc/shadow, each when absent;
# the check targets ensure the busybox/dropbear binaries are real.
os-real-image: krffs rach2 utils check-busybox check-busybox-suid check-dropbearmulti ports/doom
	sh krffs/gen_demo_key.sh
	sh krffs/gen_demo_password.sh
	$(MKIMAGE) krffs/manifests/os-real.manifest $(OS_REAL_IMAGE) 16

# The os-real disk: the same os-real tree, but on a real disk image instead of a
# RAM-disk module. A 64 MB disk with a 48 MB KRFFS partition at LBA 2048; the
# partition is larger than the 32 MB resident page-cache cap on purpose, so it can
# later prove an on-disk file system bigger than what fits in RAM at once.
os-real-disk: krffs rach2 utils check-busybox check-busybox-suid check-dropbearmulti ports/doom
	sh krffs/gen_demo_key.sh
	sh krffs/gen_demo_password.sh
	$(MKDISK) krffs/manifests/os-real.manifest $(OS_REAL_DISK) 64 48

# A BIOS-bootable disk: GRUB in the MBR and a FAT boot partition holding the
# kernel, plus a KRFFS partition with the os-real tree. It boots a real machine
# (or QEMU with no -kernel/-initrd) BIOS -> MBR -> GRUB -> kernel -> KRFFS on the
# disk. Built in a throwaway Docker container (needs Docker).
os-real-grub-disk: krffs rach2 utils check-busybox check-busybox-suid check-dropbearmulti ports/doom
	sh krffs/gen_demo_key.sh
	sh krffs/gen_demo_password.sh
	sh tools/mkdisk_grub.sh

# `make test`: the automated end-to-end pass over the test image. Boots it headless
# (-cpu max) and runs every probe, failing if any does not exit 0. The few probes
# with a special contract (fork-exit-code, stdin, faults, network) are run by hand
# under tools/harness.py; see tools/runtests.py.
# --timeout is one deadline for the whole run, not per probe, so it is stated here
# rather than left at the default: the suite grows every milestone, and a default
# sized for an older, shorter suite fails the tail as a hang on a slow host.
test: test-image
	python3 tools/runtests.py --kernel build/rach2/kernel --initrd $(TEST_IMAGE) \
	    --timeout 300

# `make test-disk`: the disk-backed page-cache integrity check. Builds a disk
# image holding blockprobe, boots disk-only with KRFFS on the real ATA disk
# through the cache (rach2.fsdisk), and runs blockprobe from the debug shell,
# passing on its "BLOCKPROBE OK" serial token. blockprobe writes more than the
# resident cache, so it exercises real eviction and writeback and fails on any
# dropped or corrupted page.
DISK_TEST_IMAGE = $(IMAGES_DIR)/disk-test.disk
test-disk: rach2 tests krffs
	$(MKDISK) krffs/manifests/disk-test.manifest $(DISK_TEST_IMAGE) 64 48
	python3 tools/harness.py --kernel build/rach2/kernel \
	    --append "rach2.fsdisk rach2.rescue" \
	    --drive "file=$(DISK_TEST_IMAGE),format=raw,if=ide,index=0,media=disk" \
	    --timeout 90 --ok "BLOCKPROBE OK" probe=blockprobe

# `make persist-test`: fsync durability across a reboot. Builds a disk holding
# persistprobe, boots it twice against the SAME image: the first boot writes and
# fsyncs /persist.txt, the second reads it back. The second boot passes only if
# fsync pushed the write through the cache to the disk, since the file would
# otherwise be gone.
PERSIST_DISK = $(IMAGES_DIR)/persist-test.disk
persist-test: rach2 tests krffs
	$(MKDISK) krffs/manifests/persist-test.manifest $(PERSIST_DISK) 64 48
	python3 tools/harness.py --kernel build/rach2/kernel \
	    --append "rach2.fsdisk rach2.rescue" \
	    --drive "file=$(PERSIST_DISK),format=raw,if=ide,index=0,media=disk" \
	    --timeout 60 --ok "PERSISTPROBE OK (wrote)" probe=persistprobe
	python3 tools/harness.py --kernel build/rach2/kernel \
	    --append "rach2.fsdisk rach2.rescue" \
	    --drive "file=$(PERSIST_DISK),format=raw,if=ide,index=0,media=disk" \
	    --timeout 60 --ok "PERSISTPROBE OK (read back)" probe=persistprobe

# `make crash-test`: prove the dirty-mount flag detects an unclean shutdown. A
# crashed boot (killed after mounting) leaves the on-disk dirty bit set for
# fsck.krffs to report; a clean halt clears it. Its own image so it cannot race
# test-disk on a shared file under `make -j`.
CRASH_TEST_IMAGE = $(IMAGES_DIR)/crash-test.disk
crash-test: rach2 tests krffs
	$(MKDISK) krffs/manifests/disk-test.manifest $(CRASH_TEST_IMAGE) 64 48
	sh tools/crash_test.sh build/rach2/kernel $(CRASH_TEST_IMAGE) build/krffs/fsck.krffs

# `make stress-test`: prove an on-disk KRFFS larger than physical RAM works. Boots
# a 64 MB disk with a 48 MB partition and runs stressprobe, which writes and
# re-reads 33 MiB (past the 32 MiB RAM cap) through the 1 MiB cache.
STRESS_DISK = $(IMAGES_DIR)/stress-test.disk
stress-test: rach2 tests krffs
	$(MKDISK) krffs/manifests/stress-test.manifest $(STRESS_DISK) 64 48
	python3 tools/harness.py --kernel build/rach2/kernel \
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

test-disk-ahci: rach2 tests krffs
	$(MKDISK) krffs/manifests/disk-test.manifest $(DISK_TEST_AHCI_IMAGE) 64 48
	python3 tools/harness.py --kernel build/rach2/kernel \
	    --append "rach2.disk=ahci rach2.fsdisk rach2.rescue" \
	    --drive "file=$(DISK_TEST_AHCI_IMAGE),format=raw,if=none,id=drv0" \
	    $(AHCI_ATTACH) \
	    --timeout 90 --ok "BLOCKPROBE OK" probe=blockprobe

persist-test-ahci: rach2 tests krffs
	$(MKDISK) krffs/manifests/persist-test.manifest $(PERSIST_AHCI_DISK) 64 48
	python3 tools/harness.py --kernel build/rach2/kernel \
	    --append "rach2.disk=ahci rach2.fsdisk rach2.rescue" \
	    --drive "file=$(PERSIST_AHCI_DISK),format=raw,if=none,id=drv0" \
	    $(AHCI_ATTACH) \
	    --timeout 60 --ok "PERSISTPROBE OK (wrote)" probe=persistprobe
	python3 tools/harness.py --kernel build/rach2/kernel \
	    --append "rach2.disk=ahci rach2.fsdisk rach2.rescue" \
	    --drive "file=$(PERSIST_AHCI_DISK),format=raw,if=none,id=drv0" \
	    $(AHCI_ATTACH) \
	    --timeout 60 --ok "PERSISTPROBE OK (read back)" probe=persistprobe

stress-test-ahci: rach2 tests krffs
	$(MKDISK) krffs/manifests/stress-test.manifest $(STRESS_AHCI_DISK) 64 48
	python3 tools/harness.py --kernel build/rach2/kernel \
	    --append "rach2.disk=ahci rach2.fsdisk rach2.rescue" \
	    --drive "file=$(STRESS_AHCI_DISK),format=raw,if=none,id=drv0" \
	    $(AHCI_ATTACH) \
	    --timeout 150 --ok "STRESSPROBE OK" \
	    "expect=Type 'help'" "send=exec /stressprobe" wait=130

# Bootable ISO images for real hardware: each embeds the kernel and one RAM-disk
# image as GRUB multiboot modules, built by grub-mkrescue in a throwaway Docker
# container (see tools/mkiso.sh). Output: build/images/<image>.iso.
isos: test-iso os-simple-iso os-real-iso
test-iso: test-image
	sh tools/mkiso.sh test
os-simple-iso: os-simple-image
	sh tools/mkiso.sh os-simple
os-real-iso: os-real-image
	sh tools/mkiso.sh os-real

# The staged capstone binaries are the REAL prerequisites of the os-real image. A
# stamp only proves its build RAN; the artifact can be deleted OR truncated afterwards
# while the stamp lingers, so these PHONY check targets (depending on the stamp so it
# builds first) run `test -s` on EVERY build and catch a missing OR zero-byte artifact.
check-busybox: $(BUSYBOX_STAMP)
	@test -s $(BUSYBOX_BIN) || { echo "busybox: $(BUSYBOX_BIN) missing or empty; rm vendor/busybox/.stamp and rebuild (make busybox)" >&2; exit 1; }

# Privilege in the os-real image climbs through /bin/su alone; busybox must stay
# unprivileged (a setuid busybox would run its BB_SUID_REQUIRE/MAYBE applets as
# root). This asserts the staged modes hold that invariant, so making busybox
# setuid without a confining config fails the build, not a boot.
check-busybox-suid:
	@python3 tools/check_busybox_suid.py krffs/manifests/os-real.manifest
check-dropbearmulti: $(DROPBEAR_STAMP)
	@test -s $(DROPBEAR_BIN) || { echo "dropbear: $(DROPBEAR_BIN) missing or empty; rm vendor/dropbear/.stamp and rebuild (make dropbear)" >&2; exit 1; }

# musl's stamp guards two artifacts, not one: the installed libc the cross links
# against, and the EXTRACTED source tree, which krffs (mkhash's crypt) and the
# mkhash seam test compile from directly. A copied worktree carries the stamp
# without either, so check both here rather than let a consumer fail later on a
# missing file with no cause named.
check-libc: $(LIBC_STAMP)
	@test -s $(LIBC_A) || { echo "libc: $(LIBC_A) missing or empty; rm vendor/musl/.stamp and rebuild (make deps)" >&2; exit 1; }
	@test -f $(MUSL_CRYPT_SRC) || { echo "libc: $(MUSL_CRYPT_SRC) missing; rm vendor/musl/.stamp and rebuild (make deps)" >&2; exit 1; }

# --- toolchain preflight + quality gates ---
doctor:
	@./tools/doctor.sh

# Lightweight quality gate (see tools/check.sh): -Werror build, scoped cppcheck,
# shellcheck, the convention grep, clang-format, and a readelf ABI check.
verify-style:
	@./tools/check.sh

# Dual-target seam tests (see tools/hosttest.sh): pure kernel logic compiled and
# run natively under AddressSanitizer + UndefinedBehaviorSanitizer. It waits on
# the vendored musl for the same reason krffs does: mkhash_hosttest names the
# musl crypt in its host-deps, and that source exists only once the tarball is
# extracted.
verify-host: check-libc
	@./tools/hosttest.sh

# --- IDE support + source-list guard ---
# clangd compilation database, generated from the build's own commands (GCC has no
# clang-style -MJ). Output: compile_commands.json at the repo root (git-ignored). The
# checked-in .clangd points clangd at the i686-elf cross GCC; see mk/Readme.md.
compile-commands:
	@./tools/gen_compile_commands.sh

# Kernel source-list guard: fail if a rach2 .c on disk is missing from C_SRC (so a new
# kernel source cannot be silently dropped from the build). Also run inside the gate.
check-sources:
	@./tools/check_sources.sh

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
# clean leaves in place). The next build rebuilds musl/busybox/dropbear from the tarballs.
distclean: clean
	rm -f $(LIBC_STAMP) $(BUSYBOX_STAMP) $(DROPBEAR_STAMP)
	rm -rf vendor/musl/install \
	       $(LIBC_TARBALL:.tar.gz=) $(BUSYBOX_TARBALL:.tar.bz2=) $(DROPBEAR_TARBALL:.tar.bz2=)
	rm -rf build

help:
	@echo "Rach2 build targets:"
	@echo "  make            kernel + the os-simple image (ish shell + simple Unix tools)"
	@echo "  make full       all the RAM-disk images"
	@echo "  make images     the RAM-disk images (test, os-simple, os-real)"
	@echo "  make test-image / os-simple-image / os-real-image   one image"
	@echo "  make os-real-disk   the os-real tree on a partitioned ATA disk image"
	@echo "  make os-real-grub-disk   a BIOS-bootable GRUB disk (needs Docker)"
	@echo "  make test       boot the test image and check every probe passes"
	@echo "  make test-disk / persist-test / crash-test / stress-test   disk integrity checks"
	@echo "  make test-disk-ahci / persist-test-ahci / stress-test-ahci   the same over AHCI/SATA"
	@echo "  make isos       a bootable ISO per image, for real hardware (needs Docker)"
	@echo "  make deps       vendored third-party (libc; DEPS=full adds busybox+dropbear)"
	@echo "  make busybox / make dropbear   one capstone's third-party build"
	@echo "  make doctor     check the toolchain (PASS/MISSING with install hints)"
	@echo "  make verify-style / verify-host   quality gates"
	@echo "  make compile-commands   regenerate compile_commands.json for clangd (IDE)"
	@echo "  make check-sources      verify the kernel C_SRC list matches the sources on disk"
	@echo "  make clean      remove the in-house build products and the images"
	@echo "  make distclean  + the vendored musl/busybox/dropbear builds (full from-scratch)"
	@echo ""
	@echo "Run an image (the build system does not run QEMU):"
	@echo "  cd rach2 && ./run.sh os-simple              # boot the os-simple image"
	@echo "  cd rach2 && ./run.sh test                   # boot the test image"
	@echo "  cd rach2 && ./run_from_disk.sh              # boot the GRUB disk (no -kernel; SSH on host 2222)"
