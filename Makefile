# Rach2 root Makefile.
#
# The build produces purpose-built RAM-disk images, each its own file under
# build/images/ so they coexist: `test` (the automated end-to-end test binaries),
# `os-simple` (a small Unix built out of this project's own code: our login,
# shell and Unix tools), and `os-real` (the same jobs taken from the code a real
# system ships: busybox, dropbear, Doom, and the tcc compiler with pdpmake to
# drive it).
#
# Quick start (run `make doctor` first to check your toolchain):
#   make            build the kernel + the os-simple image: the everyday teaching
#                   build, kept fast.
#   make full       all the RAM-disk images (test, os-simple, os-real).
#   make images     the RAM-disk images.
#   make test       boot the test image and check every probe passes.
#   make selfbuild-test   the machine builds its own kernel, utilities and shell.
#   make isos       a bootable ISO per image, for real hardware (needs Docker).
#   make deps       build the vendored third-party (libc; DEPS=full adds the
#                   busybox, dropbear, tcc and pdpmake capstones)
#   make doctor     check the toolchain (PASS/MISSING with install hints)
#   make verify-style / verify-host   the quality gates
#   make clean      remove the in-house build products and the images
#   make distclean  + the vendored musl, busybox, dropbear, tcc and pdpmake
#                   builds (full from-scratch)
#   make help       list every target
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
METAL_IMAGE     = $(IMAGES_DIR)/rach2-metal.krffs
OS_REAL_DISK    = $(IMAGES_DIR)/os-real.disk
MKIMAGE         = krffs/mkimage.sh
MKDISK          = krffs/mkdisk.sh

.PHONY: all full deps clean distclean images musl-programs test test-disk bootpart-test install-test \
        test-noentropy \
        test-image os-simple-image os-real-image os-real-disk persist-test \
        cred-persist-test \
        ping-test netconsole-test test-addressed metal-test rach2-metal diskpolicy-test \
        subset-test diskdiag-test reset-test \
        ping-test account-test shutdown-test login-test crash-test stress-test os-real-grub-disk \
        tcc-test ssh-test make-test selfbuild-test \
        test-disk-ahci test-disk-q35 persist-test-ahci stress-test-ahci rach2-metal-grub-disk \
        rach2-sealed-disk rach2-recovery authority-test \
        isos test-iso os-simple-iso os-real-iso rach2-metal-image rach2-metal-iso \
        check-busybox check-setid check-dropbearmulti check-libc check-tcc \
        check-pdpmake \
        compile-commands check-sources \
        verify-style verify-host doctor help \
        busybox dropbear tcc pdpmake $(BASE_DIRS) $(FULL_DIRS)

# Default: the kernel and the os-simple image (our login, shell and Unix tools).
all: os-simple-image

# Everything: all the RAM-disk images. Each is its own file under build/images/, so
# they coexist instead of overwriting one shared image.
full: images

# Build the vendored third-party. libc is built by default (utils link it); the
# four capstones os-real carries are added with DEPS=full (they are slow, and
# nothing but that image needs them).
deps: $(LIBC_STAMP)
ifeq ($(DEPS),full)
deps: $(BUSYBOX_STAMP) $(DROPBEAR_STAMP) $(TCC_STAMP) $(PDPMAKE_STAMP)
endif

# Friendly aliases for the third-party stamps.
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

# The pinned patches are prerequisites, not decoration: build_libc.sh applies
# them after the extract, so a tree whose stamp predates a patch would keep
# linking every program against the unpatched libc, and an edit to one would
# never be built at all.
LIBC_PATCHES = $(wildcard vendor/musl/patches/*.patch)

$(LIBC_STAMP): vendor/musl/build_libc.sh $(LIBC_TARBALL) $(LIBC_PATCHES)
	@rm -f $@ vendor/musl/install/lib/libc.a
	./vendor/musl/build_libc.sh
	@test -s vendor/musl/install/lib/libc.a || { echo "libc: build produced no libc.a" >&2; exit 1; }
	@touch $@

$(BUSYBOX_STAMP): vendor/busybox/build_busybox.sh vendor/busybox/rach2.config \
                  vendor/busybox/rach2-suid.config vendor/busybox/muslgcc \
                  $(BUSYBOX_TARBALL) $(LIBC_STAMP)
	@rm -f $@ $(BUSYBOX_BIN) $(BUSYBOX_SUID_BIN) \
	    build/vendor/busybox/applet_tables-main.h \
	    build/vendor/busybox/applet_tables-suid.h \
	    build/vendor/busybox/config-main \
	    build/vendor/busybox/config-suid
	./vendor/busybox/build_busybox.sh
	@test -s $(BUSYBOX_BIN) || { echo "busybox: build produced no $(BUSYBOX_BIN)" >&2; exit 1; }
	@touch $@

$(TCC_STAMP): vendor/tcc/build_tcc.sh vendor/tcc/rach2-output-policy.patch \
              vendor/busybox/muslgcc $(TCC_TARBALL) $(LIBC_STAMP)
	@rm -f $@ $(TCC_BIN) build/vendor/tcc/libtcc1.a \
	    build/vendor/tcc/include/stdarg.h
	./vendor/tcc/build_tcc.sh
	@test -s $(TCC_BIN) || { echo "tcc: build produced no $(TCC_BIN)" >&2; exit 1; }
	@touch $@

# pdpmake, the make that runs ON the machine. Its patches are prerequisites for
# the reason musl's are: they are applied at extraction, so a stamp that
# predates one would keep a make whose built-in rules name a compiler this image
# does not have and whose default link this system's compiler refuses.
$(PDPMAKE_STAMP): vendor/pdpmake/build_pdpmake.sh $(PDPMAKE_PATCHES) \
                  vendor/busybox/muslgcc $(PDPMAKE_TARBALL) $(LIBC_STAMP)
	@rm -f $@ $(PDPMAKE_BIN)
	./vendor/pdpmake/build_pdpmake.sh
	@test -s $(PDPMAKE_BIN) || { echo "pdpmake: build produced no $(PDPMAKE_BIN)" >&2; exit 1; }
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
test-image: krffs rach2 tests musl-programs utils ish
	$(MKIMAGE) krffs/manifests/test.manifest $(TEST_IMAGE) 10

# The os-simple image: a small Unix built out of this project's own code. init
# execs /bin/login, so this image needs the account databases; gen_demo_password.sh
# mints them when krffs/shadow is absent and does nothing when it is there, and it
# is the same file os-real stages, so one password opens either image.
os-simple-image: krffs rach2 ish utils
	sh krffs/gen_demo_password.sh
	$(MKIMAGE) krffs/manifests/os-simple.manifest $(OS_SIMPLE_IMAGE) 10

# The os-real image: a real Unix-like environment (busybox shell + coreutils,
# dropbear SSH server and client, Doom, and an /etc + /root tree). 16 MB: enough for
# the capstones and the shareware Doom WAD, and it still fits under the kernel's
# 32 MB physical-RAM cap once the kernel and its frames are accounted for (the module
# is loaded just above 1 MB). gen_demo_key.sh provisions the SSH login keys and
# gen_demo_password.sh the account passwords and /etc/shadow, each when absent;
# the check targets ensure the busybox/dropbear binaries are real.
os-real-image: krffs rach2 utils check-busybox check-tcc check-pdpmake check-setid check-dropbearmulti ports/doom
	sh krffs/gen_demo_key.sh
	sh krffs/gen_demo_password.sh
	$(MKIMAGE) krffs/manifests/os-real.manifest $(OS_REAL_IMAGE) 16

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
	python3 tools/check_diskpolicy.py $(METAL_KERNEL) read-only

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
	python3 tools/check_recovery.py $(RECOVERY_KERNEL) recovery
	python3 tools/check_recovery.py build/rach2/kernel ordinary
	python3 tools/check_diskpolicy.py $(RECOVERY_KERNEL) read-write

rach2-metal-image: krffs rach2 rach2-metal utils check-busybox check-setid check-dropbearmulti
	sh krffs/gen_demo_key.sh
	sh krffs/gen_demo_password.sh
	$(MKIMAGE) krffs/manifests/rach2-metal.manifest $(METAL_IMAGE) 4

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
	sh tools/mkdisk_grub.sh

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
	    sh tools/mkdisk_grub.sh build/rach2/kernel $(METAL_GRUB_DISK)

# `make rach2-sealed-disk`: the disk milestone 25 seals into the laptop, written
# once over a USB adapter and not removed again. Its geometry is the milestone's
# contract and every number here is checked against the built image: a 64 MiB
# FAT32 boot partition at 4 KiB clusters (one sector per cluster puts a 64 MiB
# volume past both of bootslot's bounds), a 500 MiB root, and a 64 MiB recovery
# partition whose kernel and RAM image GRUB can reach when partition 1 cannot be
# read at all. The image is 630 MiB, 1290240 sectors.
#
# This target is the geometry only. The boot menu, the GRUB core slots, the
# network configuration and the recovery contents are added by the phases that
# own them; until then it boots the same machine the metal disk does.
SEALED_DISK = build/images/rach2-sealed.disk
rach2-sealed-disk: krffs rach2 utils check-busybox check-tcc check-pdpmake check-setid check-dropbearmulti ports/doom
	sh krffs/gen_demo_key.sh
	sh krffs/gen_demo_password.sh
	RACH2_DISK_MANIFEST=krffs/manifests/rach2-metal-disk.manifest \
	RACH2_DISK_BOOT_MB=64 \
	RACH2_DISK_CLUSTER=8 \
	RACH2_DISK_KRFFS_MB=500 \
	RACH2_DISK_RECOVERY_MB=64 \
	RACH2_DISK_APPEND="rach2.disk=ahci rach2.ip=192.168.0.105/24 rach2.gateway=192.168.0.1 rach2.netconsole" \
	    sh tools/mkdisk_grub.sh build/rach2/kernel $(SEALED_DISK)

# `make test`: the automated end-to-end pass over the test image. Boots it headless
# (-cpu max) and runs every probe, failing if any does not exit 0. The few probes
# with a special contract (fork-exit-code, stdin, faults, network) are run by hand
# under tools/harness.py; see tools/runtests.py.
# --timeout is one deadline for the whole run, not per probe, so it is stated here
# rather than left at the default: the suite grows every milestone, and a default
# sized for an older, shorter suite fails the tail as a hang on a slow host.
# --steptmo is the per-probe window, and it is stated here for one probe:
# musl/pwauthmusl waits out the credential lock's real ten-second retry budget
# twice, so tens of seconds is its GREEN path. Only a failing probe ever waits a
# step window out, so widening it costs a passing run nothing.
test: test-image
	python3 tools/runtests.py --kernel build/rach2/kernel --initrd $(TEST_IMAGE) \
	    --timeout 300 --steptmo 60

# `make test-addressed`: the same suite, with the network named on the boot line
# instead of compiled in. The address given is the one SLIRP already provides,
# so every probe must pass exactly as it does without it; what is under test is
# that `rach2.ip=` and `rach2.gateway=` reach ARP, IP, TCP, the sockets and the
# boot line, and that naming the current network changes nothing.
test-addressed: test-image
	python3 tools/runtests.py --kernel build/rach2/kernel --initrd $(TEST_IMAGE) \
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
test-noentropy: test-image
	python3 tools/harness.py --kernel build/rach2/kernel --initrd $(TEST_IMAGE) \
	    --qemu-arg=-cpu --qemu-arg=qemu64 \
	    --timeout 90 --ok "SALTNOENT OK" \
	    "expect=Type 'help'" "send=exec /musl/saltnoent" wait=20
	python3 tools/harness.py --kernel build/rach2/kernel --initrd $(TEST_IMAGE) \
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
test-disk: rach2 tests krffs
	$(MKDISK) krffs/manifests/disk-test.manifest $(DISK_TEST_IMAGE) 64 48
	python3 tools/harness.py --kernel build/rach2/kernel \
	    --append "rach2.fsdisk rach2.rescue" \
	    --drive "file=$(DISK_TEST_IMAGE),format=raw,if=ide,index=0,media=disk" \
	    --timeout 90 --ok "BLOCKPROBE OK" probe=blockprobe

# `make bootpart-test`: /proc/cmdline and the level-one disk devices. Three
# boots of one disk: as mkdisk built it there is a single KRFFS partition, so
# /dev/bootpart must be absent from stat and open; after tools/mkbootfixture.py
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
bootpart-test: rach2 tests krffs
	$(MKDISK) krffs/manifests/disk-test.manifest $(BOOTPART_TEST_IMAGE) 64 48
	python3 tools/harness.py --kernel build/rach2/kernel \
	    --append "rach2.fsdisk rach2.rescue" \
	    --drive "file=$(BOOTPART_TEST_IMAGE),format=raw,if=ide,index=0,media=disk" \
	    --timeout 90 --ok "BOOTPROBE ABSENT OK" probe=bootprobe
	python3 tools/mkbootfixture.py $(BOOTPART_TEST_IMAGE)
	python3 tools/harness.py --kernel build/rach2/kernel \
	    --append "rach2.fsdisk rach2.rescue rach2.bootprobe=fixture" \
	    --drive "file=$(BOOTPART_TEST_IMAGE),format=raw,if=ide,index=0,media=disk" \
	    --timeout 90 --ok "BOOTPROBE OK" probe=bootprobe
	python3 tools/harness.py --kernel build/rach2/kernel --initrd $(TEST_IMAGE) \
	    --append "rach2.rescue rach2.bootprobe=ramroot" \
	    --drive "file=$(BOOTPART_TEST_IMAGE),format=raw,if=ide,index=0,media=disk" \
	    --timeout 90 --ok "BOOTPROBE RAMROOT OK" probe=bootprobe
	python3 tools/mkwidefixture.py $(WIDE_TEST_IMAGE)
	python3 tools/harness.py --kernel build/rach2/kernel --initrd $(TEST_IMAGE) \
	    --append "rach2.disk=ahci rach2.rescue rach2.diskdiag rach2.bootprobe=wide" \
	    --drive "file=$(WIDE_TEST_IMAGE),format=raw,if=none,id=drv0" \
	    $(AHCI_ATTACH) \
	    --timeout 180 --ok "BOOTPROBE WIDE OK" probe=bootprobe

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
authority-test: rach2 rach2-recovery tests krffs
	python3 tools/mkwidefixture.py $(AUTH_TEST_IMAGE)
	python3 tools/check_recovery.py build/rach2/kernel ordinary
	python3 tools/check_recovery.py $(RECOVERY_KERNEL) recovery
	python3 tools/harness.py --kernel build/rach2/kernel --initrd $(TEST_IMAGE) \
	    --append "rach2.disk=ahci rach2.rescue rach2.diskdiag rach2.bootprobe=auth-ordinary" \
	    --drive "file=$(AUTH_TEST_IMAGE),format=raw,if=none,id=drv0" \
	    $(AHCI_ATTACH) \
	    --timeout 180 --ok "BOOTPROBE AUTH ORDINARY OK" probe=bootprobe
	python3 tools/harness.py --kernel $(RECOVERY_KERNEL) --initrd $(TEST_IMAGE) \
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
install-test: rach2 utils krffs check-busybox check-tcc check-pdpmake check-setid check-dropbearmulti ports/doom
	@mkdir -p $(IMAGES_DIR)
	sh krffs/gen_demo_password.sh
	sh krffs/gen_demo_key.sh
	sh tools/mkdisk_grub.sh build/rach2/kernel $(INSTALL_TEST_DISK)
	sh tools/install_test.sh $(INSTALL_TEST_DISK) \
	    "`cat krffs/rach2_demo_password`"

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
cred-persist-test: rach2 musl-programs krffs
	$(MKDISK) krffs/manifests/cred-persist-test.manifest $(CRED_PERSIST_DISK) 64 48
	python3 tools/harness.py --kernel build/rach2/kernel \
	    --append "rach2.fsdisk rach2.rescue" \
	    --drive "file=$(CRED_PERSIST_DISK),format=raw,if=ide,index=0,media=disk" \
	    --timeout 60 --ok "CREDDISK OK (wrote)" \
	    "expect=Type 'help'" "send=exec /creddisk" wait=15
	python3 tools/harness.py --kernel build/rach2/kernel \
	    --append "rach2.fsdisk rach2.rescue" \
	    --drive "file=$(CRED_PERSIST_DISK),format=raw,if=ide,index=0,media=disk" \
	    --timeout 60 --ok "CREDDISK OK (released)" \
	    "expect=Type 'help'" "send=exec /creddisk" wait=15
	python3 tools/harness.py --kernel build/rach2/kernel \
	    --append "rach2.fsdisk rach2.rescue" \
	    --drive "file=$(CRED_PERSIST_DISK),format=raw,if=ide,index=0,media=disk" \
	    --timeout 60 --ok "CREDDISK OK (read back)" \
	    "expect=Type 'help'" "send=exec /creddisk" wait=15

# `make crash-test`: prove the dirty-mount flag detects an unclean shutdown. A
# crashed boot (killed after mounting) leaves the on-disk dirty bit set for
# fsck.krffs to report; a clean halt clears it. Its own image so it cannot race
# test-disk on a shared file under `make -j`.
CRASH_TEST_IMAGE = $(IMAGES_DIR)/crash-test.disk
# `make ping-test`: the ICMP path against the SLIRP gateway, which is why it is
# its own target rather than a probe in the suite. The explicit wait= is
# required: after its last step harness.py drains only --grace and then
# kills QEMU, while the timeout case alone costs four seconds and the child the
# probe forks lives three to eight, so without it the token would arrive after
# QEMU was dead.
ping-test: rach2 tests krffs test-image
	python3 tools/harness.py --kernel build/rach2/kernel \
	    --initrd $(TEST_IMAGE) --qemu-arg=-cpu --qemu-arg=max \
	    --timeout 120 --ok "PINGTEST OK" \
	    "expect=Type 'help'" "send=exec /pingtest" wait=60

# `make netconsole-test`: the kernel log on the wire. The machine this milestone
# targets has no serial port, so the boot log has to leave over Ethernet or not
# at all. QEMU's filter-dump writes every frame the guest sends to a libpcap
# file; the boot below asks for netconsole on the command line, and the checker
# holds the frames to the header's contract and requires a line printed BEFORE
# the network device came up, which is what proves the pre-transmit buffer was
# carried rather than dropped.
NETCONSOLE_PCAP = $(IMAGES_DIR)/netconsole.pcap
netconsole-test: rach2 tests krffs test-image
	@rm -f $(NETCONSOLE_PCAP)
	python3 tools/harness.py --kernel build/rach2/kernel \
	    --initrd $(TEST_IMAGE) --qemu-arg=-cpu --qemu-arg=max \
	    --qemu-arg=-object \
	    --qemu-arg=filter-dump,id=nc0,netdev=net0,file=$(NETCONSOLE_PCAP) \
	    --append "rach2.netconsole" \
	    --timeout 90 --ok "Type 'help'" "expect=Type 'help'" wait=3
	python3 tools/netconsole_check.py $(NETCONSOLE_PCAP) --min-frames 4 \
	    --expect "Initializing system components" \
	    --expect "Network... OK" \
	    --expect "netconsole: broadcast"

# `make reset-test`: each reset mechanism on its own. The kernel tries three,
# most of the machine first, and the order was chosen for a laptop whose
# chipset register may not act; the risk in that choice is that only the first
# one is ever exercised, and nobody finds out the later ones do not work until
# the machine they were written for refuses to restart. So each is booted with
# the ones before it left out, and each must still restart the machine.
reset-test: rach2 tests krffs test-image
	sh tools/reset_test.sh build/rach2/kernel $(TEST_IMAGE)

# `make diskdiag-test`: a disk read without being mounted and without being
# written. Three boots of one freshly built image: the ordinary kernel mounting
# it, which must change it (otherwise the comparisons below prove nothing); the
# read-only kernel reading it over SSH with nothing typed at the console, where
# every sector hashed on the machine must equal the same offset hashed here;
# and the same kernel with no drive at all.
diskdiag-test: rach2 rach2-metal rach2-metal-image
	sh tools/diskdiag_test.sh build/rach2/kernel $(METAL_KERNEL) $(METAL_IMAGE) 2224

# `make subset-test`: the kernel reaches a shell with any subset of its optional
# devices switched off. On a machine with no serial port, a boot that stops has
# no diagnostic but the screen, and the only way to find which probe stopped it
# is to switch them off one at a time from the GRUB command line. That is only
# useful if each switch actually works, so each is booted here.
subset-test: rach2 tests krffs test-image
	@for opt in "" "rach2.noaudio" "rach2.nomouse" "rach2.nonet" "rach2.nodisk" \
	            "rach2.noaudio rach2.nomouse rach2.nonet rach2.nodisk"; do \
	    echo "  subset-test: booting with [$$opt]"; \
	    python3 tools/harness.py --kernel build/rach2/kernel \
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
diskpolicy-test: rach2 rach2-metal
	sh tools/diskpolicy_test.sh build/rach2/kernel $(METAL_KERNEL)

# `make metal-test`: the metal image reached over SSH with nobody at its
# console. This is the rehearsal for the Lenovo: the machine is given power and
# an address on its command line, and nothing is typed at it. What that proves
# is everything that has to happen by itself for a machine across the room to be
# usable, so the address configuration, init-metal's supervision of dropbear,
# key authentication, a pty, and a file crossing intact each way are all one
# test. It kills the server once as well, because a supervisor that starts a
# daemon and does not restart it has not been tested.
metal-test: rach2-metal rach2-metal-image
	sh tools/metal_test.sh $(METAL_KERNEL) $(METAL_IMAGE) 2223

# `make account-test`: accounts created, authenticated, changed and deleted on
# both images, since both have account tools now and they are different code.
# os-real gets busybox's over three boots, so what is asserted is what reached
# the platter and not what one process had in its cache; os-simple gets this
# project's own in RAM, where there is no `deluser` to finish with. Both
# authenticate from an unprivileged account, since root's `su` asks for no
# password and would pass over a wrong hash and an unusable set-id `passwd`
# alike.
ACCOUNT_TEST_DISK = $(IMAGES_DIR)/account-test.disk

account-test: rach2 utils krffs busybox ish check-tcc check-pdpmake
	sh krffs/gen_demo_key.sh
	sh krffs/gen_demo_password.sh
	$(MKDISK) krffs/manifests/os-real.manifest $(ACCOUNT_TEST_DISK) 64 48
	sh tools/account_test.sh build/rach2/kernel $(ACCOUNT_TEST_DISK) \
	    "`cat krffs/rach2_demo_password`" real
	$(MKIMAGE) krffs/manifests/os-simple.manifest $(OS_SIMPLE_IMAGE) 10
	sh tools/account_test.sh build/rach2/kernel $(OS_SIMPLE_IMAGE) \
	    "`cat krffs/rach2_demo_password`" simple

# `make tcc-test`: the machine compiling a program on itself. Boots os-real from
# a disk as the unprivileged `test` account, writes a C file at the prompt,
# compiles it, runs the result, and then asks for the four kinds of program this
# system cannot run and requires each to be refused with its reason and to leave
# no output behind. The boot carries `rach2.trace=enosys`, so a syscall the
# compiler wants and this kernel lacks is named on the console rather than
# turning into a mysterious failure.
TCC_TEST_DISK = $(IMAGES_DIR)/tcc-test.disk

tcc-test: rach2 utils krffs check-busybox check-tcc check-pdpmake check-setid check-dropbearmulti ports/doom
	sh krffs/gen_demo_key.sh
	sh krffs/gen_demo_password.sh
	$(MKDISK) krffs/manifests/os-real.manifest $(TCC_TEST_DISK) 64 48
	sh tools/tcc_test.sh build/rach2/kernel $(TCC_TEST_DISK) \
	    "`cat krffs/rach2_demo_password`" "`cat krffs/rach2_test_password`"

# `make make-test`: the machine building a program on itself, and the claim a
# compiler alone cannot make: that a second build does nothing, that touching
# one source rebuilds one object, and that the answer survives a reboot. Two
# boots of one disk, since the reboot is half the point.
MAKE_TEST_DISK = $(IMAGES_DIR)/make-test.disk

make-test: rach2 utils krffs check-busybox check-tcc check-pdpmake check-setid check-dropbearmulti ports/doom
	@mkdir -p $(IMAGES_DIR)
	sh krffs/gen_demo_password.sh
	$(MKDISK) krffs/manifests/os-real.manifest $(MAKE_TEST_DISK) 64 48
	sh tools/make_test.sh build/rach2/kernel $(MAKE_TEST_DISK) \
	    "`cat krffs/rach2_test_password`"

# `make selfbuild-test`: the machine builds its own kernel. Boots os-real from
# a disk copy as the unprivileged test account, copies /src/rach2 home, and
# runs the staged make and compiler over the staged guest.mk, twice from clean
# workspaces; the two kernels must hash equal. Every wait is bounded, and the
# whole run carries one deadline over the per-step ones.
SELFBUILD_DISK = $(IMAGES_DIR)/selfbuild-test.disk

selfbuild-test: rach2 utils krffs test-image check-busybox check-tcc check-pdpmake check-setid check-dropbearmulti ports/doom
	@mkdir -p $(IMAGES_DIR)
	sh krffs/gen_demo_password.sh
	$(MKDISK) krffs/manifests/os-real.manifest $(SELFBUILD_DISK) 64 48
	sh tools/selfbuild_test.sh build/rach2/kernel $(SELFBUILD_DISK) \
	    "`cat krffs/rach2_test_password`" "`cat krffs/rach2_demo_password`"

# `make ssh-test`: the SSH capstone with nobody at the keyboard. Boots os-real
# from a disk with the port forwarded, starts dropbear from the console session
# the way the Readme's recipe does, then logs in twice: root by key, and the
# unprivileged account by password, which is the half that reaches /etc/shadow.
# Every wait is bounded, so a hang fails instead of sitting there.
SSH_TEST_DISK = $(IMAGES_DIR)/ssh-test.disk
SSH_TEST_PORT ?= 2222

ssh-test: rach2 utils krffs check-busybox check-tcc check-pdpmake check-setid check-dropbearmulti ports/doom
	sh krffs/gen_demo_key.sh
	sh krffs/gen_demo_password.sh
	$(MKDISK) krffs/manifests/os-real.manifest $(SSH_TEST_DISK) 64 48
	sh tools/ssh_test.sh build/rach2/kernel $(SSH_TEST_DISK) \
	    "`cat krffs/rach2_demo_password`" "`cat krffs/rach2_test_password`" \
	    $(SSH_TEST_PORT)

# `make shutdown-test`: userland stopping the machine, on a disk fixture, with
# every claim of a stop asserted on the QEMU process ending rather than on a
# token, and every image checked by offline fsck afterwards.
SHUTDOWN_TEST_DISK = $(IMAGES_DIR)/shutdown-test.disk

shutdown-test: rach2 utils krffs busybox
	sh krffs/gen_demo_password.sh
	$(MKDISK) krffs/manifests/shutdown-test.manifest $(SHUTDOWN_TEST_DISK) 64 48
	sh tools/shutdown_test.sh build/rach2/kernel $(SHUTDOWN_TEST_DISK) \
	    build/krffs/fsck.krffs "`cat krffs/rach2_demo_password`"

# `make login-test`: prove the console cannot fall back to an unauthenticated
# shell. Two images differing only in the init they declare, neither staging
# /bin/login, plus a rescue boot of the real image, which is the way back into
# one that will not offer a prompt.
LOGIN_TEST_STRICT = $(IMAGES_DIR)/login-test-strict.krffs
LOGIN_TEST_SHELL  = $(IMAGES_DIR)/login-test-shell.krffs

login-test: rach2 utils krffs busybox os-real-image
	$(MKIMAGE) krffs/manifests/login-test-strict.manifest $(LOGIN_TEST_STRICT) 10
	$(MKIMAGE) krffs/manifests/login-test-shell.manifest $(LOGIN_TEST_SHELL) 10
	sh tools/login_test.sh build/rach2/kernel $(LOGIN_TEST_STRICT) \
	    $(LOGIN_TEST_SHELL) $(OS_REAL_IMAGE) root "`cat krffs/rach2_demo_password`"

crash-test: rach2 tests krffs
	$(MKDISK) krffs/manifests/disk-test.manifest $(CRASH_TEST_IMAGE) 64 48
	sh tools/crash_test.sh build/rach2/kernel $(CRASH_TEST_IMAGE) build/krffs/fsck.krffs

# `make stress-test`: prove an on-disk KRFFS larger than physical RAM works. Boots
# a 64 MB disk with a 48 MB partition and runs stressprobe, which writes and
# re-reads 33 MiB (past the 32 MiB RAM cap) through the 4 MiB cache.
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

# `make test-disk-q35`: the same disk proof on the q35 machine, whose AHCI
# controller is built in at 0:31.2, function 2 of the multi-function device 31.
# That is the laptop's exact topology, and no other test exercises it: every
# other AHCI test attaches a separate controller that lands at function 0, so a
# scan that never walked past function 0 would pass all of them and fail on
# the real machine.
test-disk-q35: rach2 tests krffs
	$(MKDISK) krffs/manifests/disk-test.manifest $(DISK_TEST_AHCI_IMAGE) 64 48
	python3 tools/harness.py --kernel build/rach2/kernel \
	    --append "rach2.disk=ahci rach2.fsdisk rach2.rescue" \
	    --drive "file=$(DISK_TEST_AHCI_IMAGE),format=raw,if=none,id=drv0" \
	    --qemu-arg=-machine --qemu-arg=q35 --qemu-arg=-cpu --qemu-arg=max \
	    --qemu-arg=-device --qemu-arg=ide-hd,drive=drv0,bus=ide.0 \
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
isos: test-iso os-simple-iso os-real-iso rach2-metal-iso
test-iso: test-image
	sh tools/mkiso.sh test
os-simple-iso: os-simple-image
	sh tools/mkiso.sh os-simple
os-real-iso: os-real-image
	sh tools/mkiso.sh os-real
rach2-metal-iso: rach2-metal rach2-metal-image
	sh tools/mkiso.sh rach2-metal

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
check-pdpmake: $(PDPMAKE_STAMP)
	@test -s $(PDPMAKE_BIN) || { \
	    echo "pdpmake: $(PDPMAKE_BIN) missing or empty; rm vendor/pdpmake/.stamp and rebuild (make pdpmake)" >&2; \
	    exit 1; }
	@python3 tools/check_elf.py $(PDPMAKE_BIN)

check-tcc: $(TCC_STAMP)
	@for f in $(TCC_BIN) build/vendor/tcc/libtcc1.a \
	          build/vendor/tcc/include/stdarg.h \
	          build/vendor/tcc/include/stddef.h \
	          build/vendor/tcc/include/stdbool.h \
	          build/vendor/tcc/include/float.h \
	          build/vendor/tcc/include/varargs.h; do \
	    test -s "$$f" || { echo "tcc: $$f missing or empty; rm vendor/tcc/.stamp and rebuild (make tcc)" >&2; exit 1; }; \
	done
	@python3 tools/check_elf.py --selftest
	@python3 tools/check_elf.py $(TCC_BIN)
	@for m in "must be linked with -static" "run is not available" \
	          "shared is not available" "must be an ELF"; do \
	    strings $(TCC_BIN) | grep -q -- "$$m" || { echo "tcc: the shipped compiler does not carry the output policy ($$m); rm vendor/tcc/.stamp and rebuild" >&2; exit 1; }; \
	done

check-busybox: $(BUSYBOX_STAMP)
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
	@python3 tools/check_setid.py
	@python3 tools/check_setid.py --applets
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
# clean leaves in place). The next build rebuilds musl/busybox/dropbear/tcc/pdpmake
# from the tarballs. A stamp left behind while its binary went with build/ is the
# failure this list exists to prevent: the next build would skip the work and
# fail at the check instead.
distclean: clean
	rm -f $(LIBC_STAMP) $(BUSYBOX_STAMP) $(DROPBEAR_STAMP) $(TCC_STAMP) \
	      $(PDPMAKE_STAMP)
	rm -rf vendor/musl/install \
	       $(LIBC_TARBALL:.tar.gz=) $(BUSYBOX_TARBALL:.tar.bz2=) \
	       $(DROPBEAR_TARBALL:.tar.bz2=) $(TCC_TARBALL:.tar.bz2=) \
	       $(PDPMAKE_TARBALL:.tgz=)
	rm -rf build

help:
	@echo "Rach2 build targets:"
	@echo "  make            kernel + the os-simple image (our login, shell and tools)"
	@echo "  make full       all the RAM-disk images"
	@echo "  make images     the RAM-disk images (test, os-simple, os-real)"
	@echo "  make test-image / os-simple-image / os-real-image   one image"
	@echo "  make os-real-disk   the os-real tree on a partitioned ATA disk image"
	@echo "  make os-real-grub-disk   a BIOS-bootable GRUB disk (needs Docker)"
	@echo "  make rach2-sealed-disk   the sealed laptop disk: 64 MiB boot, 500 MiB root, 64 MiB recovery"
	@echo "  make rach2-recovery      the recovery kernel, the only build that can write the whole disk"
	@echo "  make authority-test  only a recovery kernel writes the whole disk"
	@echo "  make test       boot the test image and check every probe passes"
	@echo "  make test-noentropy   the same image on a CPU with no RDSEED/RDRAND: the salt refusal"
	@echo "  make test-disk / persist-test / crash-test / stress-test   disk integrity checks"
	@echo "  make bootpart-test  the /dev/bootpart and /dev/grubcore windows and /proc/cmdline"
	@echo "  make install-test   the machine installs its own kernel (Docker)"
	@echo "  make cred-persist-test   a credential replacement read back after a reboot"
	@echo "  make ping-test  the ICMP path: replies, the timeout, and who else runs while it waits"
	@echo "  make netconsole-test  the boot log leaves over Ethernet, captured and checked"
	@echo "  make test-addressed   the same suite with the network named on the boot line"
	@echo "  make metal-test  the metal image reached over SSH with nobody at its console"
	@echo "  make diskpolicy-test  the read-only kernel leaves a disk byte for byte"
	@echo "  make subset-test  the kernel boots with any subset of its optional devices"
	@echo "  make diskdiag-test  a disk read over SSH without mounting it or writing a byte"
	@echo "  make reset-test  each of the three restart mechanisms on its own"
	@echo "  make test-disk-q35  the disk proof on the laptop's exact PCI topology"
	@echo "  make tcc-test    the machine compiles a program on itself"
	@echo "  make make-test   the machine builds one, and rebuilds only what changed"
	@echo "  make selfbuild-test   the machine builds its own kernel (twice, hash-equal), utilities and shell"
	@echo "  make ssh-test    the SSH capstone, both logins, unattended"
	@echo "  make account-test   accounts created, authenticated and deleted, on both images"
	@echo "  make shutdown-test  userland stops the machine, and only root can"
	@echo "  make login-test the console cannot fall back to an unauthenticated shell"
	@echo "  make test-disk-ahci / persist-test-ahci / stress-test-ahci   the same over AHCI/SATA"
	@echo "  make isos       a bootable ISO per image, for real hardware (needs Docker)"
	@echo "  make rach2-metal-image / rach2-metal-iso   the RAM-only image the Lenovo boots"
	@echo "  make deps       vendored third-party (libc; DEPS=full adds the four capstones)"
	@echo "  make busybox / dropbear / tcc / pdpmake   one capstone's third-party build"
	@echo "  make check-tcc / check-pdpmake   the compiler and make an image will carry"
	@echo "  make doctor     check the toolchain (PASS/MISSING with install hints)"
	@echo "  make verify-style / verify-host   quality gates"
	@echo "  make compile-commands   regenerate compile_commands.json for clangd (IDE)"
	@echo "  make check-sources      verify the kernel C_SRC list matches the sources on disk"
	@echo "  make clean      remove the in-house build products and the images"
	@echo "  make distclean  + the vendored musl, busybox, dropbear, tcc and pdpmake builds"
	@echo ""
	@echo "Run an image (the build system does not run QEMU):"
	@echo "  cd rach2 && ./run.sh os-simple              # boot the os-simple image"
	@echo "  cd rach2 && ./run.sh test                   # boot the test image"
	@echo "  cd rach2 && ./run_from_disk.sh              # boot the GRUB disk (no -kernel; SSH on host 2222)"
