# tests/system/suites.mk: how each test suite is built and run.
#
# The root Makefile includes this file. That keeps one dependency graph, which
# is what lets `tests/run authority` build the recovery kernel and the metal
# image on a clean tree: those variants exist for suites alone, and a separate
# make would have to rebuild every one of them or silently use a stale one.
#
# What belongs here: the `run-NAME` target of every suite in tests/registry,
# the disks, captures, logs and ports those suites write, the ten test tools
# under build/tests/system/, and `prepare-all`, which is the union the pooled
# runner builds once before it starts any suite.
#
# What does not: anything the machine itself ships. The kernel, the userland,
# the three bootable images, the guest probe programs staged into them, and the
# validators that judge a published artifact all stay in the root, because they
# are the product rather than the means of checking it.
#
# A suite target may assume the product is built: `tests/run` builds it and
# `prepare-all` names it. A suite target names only its own fixtures and the
# tools it invokes.

# The architecture the suites boot, ARCH (mk/arch.mk): its build tree and its
# emulator, which the recipes name and which the shell suites and the runners
# read from the environment, RACH2_QEMU for the emulator, so that a suite
# written once boots i686 on qemu-system-i386 and x86-64 on
# qemu-system-x86_64.
export RACH2_ARCH := $(ARCH)
export ARCH_BUILD
export RACH2_QEMU := $(ARCH_QEMU)

# The kernel every suite boots directly; tests/run points it at another
# component's kernel under RACH2_KERNEL (see ubsan-kernel below). Not
# KERNEL, which rach2/Makefile uses for its own output name and a sub-make
# would inherit. The disks the suites make carry $(ARCH_BUILD)/rach2/kernel:
# a checked kernel does not fit the disk's kernel slot, and what a disk suite
# proves is the boot path.
BOOT_KERNEL ?= $(ARCH_BUILD)/rach2/kernel

# Where the artifacts of etude's machine cases (tests/cases) are made, beyond
# the product's: under it for both architectures, and under the same path in
# $(ARCH_BUILD) for one, as the cases' preparation names them.
CASE_DIR = build/tests/cases/machine
ARCH_CASE_DIR = $(ARCH_BUILD)/tests/cases/machine

# The targets of the pattern rule of a maker that writes several files under
# build/ at once: the files, each with its leading `build` as the stem `%`.
# Make runs a pattern rule's recipe once for all the targets it has with one
# stem, where GNU Make 3.81, the make of macOS, has no grouped targets (`&:`).
# Under 3.81 a file that a pattern rule makes is intermediate when no rule
# names it explicitly, and make removes it when it ends; so where one such
# rule reads the files of another, they are explicit prerequisites of its own.
together = $(addprefix %/,$(patsubst build/%,%,$(1)))

# No file is named `run-<suite>`, so every suite target is phony. The names are
# read from the registry rather than written out again: `tests/run style`
# already requires the registry and the targets in this file to name the same
# suites, so a list derived from one cannot fall behind the other.
.PHONY: prepare-all prepare-images prepare-tools components harness-kernel \
        $(addprefix run-,$(shell awk -F'|' '!/^#/ && NF > 1 { print $$1 }' tests/registry))

# The card that the boots of the targets below attach, which the runner and
# the harness both take as --device: the RTL8139 unless NIC names another.
# The host ports the suites forward. They are named here because two suites
# sharing one cannot both run, and a number buried in a recipe is a collision
# nobody sees until the two are scheduled together.
#
# The table keeps the suites of one tree apart; PORT_OFFSET keeps the trees
# of one host apart. The testing host runs several trees at once, the two ci
# runners and each person's clone, and two of them running one suite would
# forward one port. So tests/run claims a block of ports for its run before
# it starts anything: block N is the lock file /tmp/rach2-ports.N, which
# every account can lock, the lowest free block of ten is taken, and the run
# exports RACH2_PORT_OFFSET as N * 1000 while it holds the lock. Every port
# below is the table's number plus that offset. Within a block the table's
# numbers are 2225 and 2296 to 2304, and etude's set driver gives the machine
# cases of tests/cases 2400 to 2499 (tools/etude/Readme.md, The set driver),
# so no port of one block is a port of another, and the highest, 2499 + 9000,
# is below the ephemeral ports. A make run without tests/run has no offset
# and forwards the table's numbers.
PORT_OFFSET     := $(or $(RACH2_PORT_OFFSET),0)
port             = $(shell echo $$(($(1) + $(PORT_OFFSET))))
GNU_PORT        := $(call port,2296)
NIC             ?= rtl8139
NIC_DEV          = --device $(NIC),netdev=net0
# The suites' wall-clock limit tool (see its rule below).
DEADLINE        = build/tests/system/deadline
PCAPCHECK       = build/tests/system/pcapcheck
MKFIXTURE       = build/tests/system/mkfixture
HOSTPEER        = build/tests/system/hostpeer
WAVCHECK        = build/tests/system/wavcheck
HARNESS         = build/tests/system/harness
RUNTESTS        = build/tests/system/runtests
PTYDRIVE        = build/tests/system/ptydrive
RUNUTIL         = build/tests/system/runutil
# The driver of etude-guest over the serial console (its rule below).
ETUDESERIAL     = build/tests/system/etudeserial
# The reader of a framebuffer console's text off QEMU's screen dump, which
# the uefi suite runs (tests/system/runner/screentext.c), with the kernel's
# own font compiled beside it.
SCREENTEXT      = build/tests/system/screentext
# The test-side disk tool, named here because a prerequisite list expands
# its variables where it is read; its rule is with the suite that uses it.
# The disk and stick tools and the sticks themselves are the product's, in
# the root Makefile and mk/images.mk.
GROWSET         = build/tools/inspect/growset
INSTALL_TEST_STICK = $(IMAGES_DIR)/install-test-stick.img
INSTALLER_MBR_DISK = $(IMAGES_DIR)/installer-test-mbr.disk
GROW_GPT_DISK = $(IMAGES_DIR)/grow-test-gpt.disk
# The deadline tool: the suites' wall-clock limit, which kills the process
# group at the deadline. It is built with the host compiler like the KRFFS
# tools.
$(DEADLINE): tests/system/runner/deadline.c
	@mkdir -p $(dir $@)
	$(HOST_LINK)
$(SCREENTEXT): tests/system/runner/screentext.c rach2/sys/display/vga_font.c rach2/sys/display/vga_font.h
	@mkdir -p $(dir $@)
	$(HOST_LINK)
# The capture checker (tests/system/checkers/pcapcheck.c): what the guest put on
# the wire, judged from QEMU's filter-dump file.
$(PCAPCHECK): tests/system/checkers/pcapcheck.c
	@mkdir -p $(dir $@)
	$(HOST_LINK)
# The disk fixtures (tests/system/fixtures/mkfixture.c): the bootprobe partition
# pattern and the 260 GB sparse disk.
$(MKFIXTURE): tests/system/fixtures/mkfixture.c
	@mkdir -p $(dir $@)
	$(HOST_LINK)
# The FAT32 fixture builder and its volume check (tests/host/mkfatfixture.c):
# the suites walk a volume the kernel wrote with `--check`.
MKFATFIXTURE = build/tests/host/mkfatfixture
$(MKFATFIXTURE): tests/host/mkfatfixture.c
	@mkdir -p $(dir $@)
	$(HOST_LINK)
# The host end of a connection (tests/system/runner/hostpeer.c): the echo
# server the socket suite sends to.
$(HOSTPEER): tests/system/runner/hostpeer.c
	@mkdir -p $(dir $@)
	$(HOST_LINK)
# What a machine played, measured off the recording QEMU's wav backend writes
# (tests/system/checkers/wavcheck.c).
$(WAVCHECK): tests/system/checkers/wavcheck.c
	@mkdir -p $(dir $@)
	$(HOST_LINK)
# The three runners (tests/system/runner/): the serial harness every suite
# drives, the probe runner one boot scores, and the pty driver the SSH suites
# answer their password prompts through.
$(HARNESS): tests/system/runner/harness.c
	@mkdir -p $(dir $@)
	$(HOST_LINK)
$(RUNTESTS): tests/system/runner/runtests.c tests/clockref.h
	@mkdir -p $(dir $@)
	$(HOST_LINK)
$(PTYDRIVE): tests/system/runner/ptydrive.c
	@mkdir -p $(dir $@)
	$(HOST_LINK)
$(RUNUTIL): tests/system/runner/runutil.c
	@mkdir -p $(dir $@)
	$(HOST_LINK)
# The driver of etude-guest (tests/system/runner/etudeserial.c), with the
# frame codec that etude-guest itself is built with.
$(ETUDESERIAL): tests/system/runner/etudeserial.c tools/etude/guest/wire.c utils/sha256.c \
                tools/etude/guest/wire.h tools/etude/include/etude/frame.h utils/sha256.h
	@mkdir -p $(dir $@)
	$(HOST_LINK) -I tools/etude/guest -I tools/etude/include -I utils
# `tests/run default`: every guest probe, each in a boot of its own on the
# test image. They are the cases of tests/cases/guest.default.json, which
# etude runs, and have no target here: guest cases that run under etude-guest,
# and machine cases, whose scenario (tests/cases/machine/default_scenario.c)
# runs a probe as the debug shell's transient init on the console. The runner
# tests/system/runner/runtests.c, which boots the probes in one machine, is
# kept for the proofs of tests/system/default_probe.sh.
# `tests/run etudeguest`: etude-guest on the test image's serial console, as
# the debug shell's designated init, without the debug-exit port. The driver
# stores the image's /hello and /ish without a transfer and utils' `yes` with
# one, and requires a pass, an intended failure, a repeated command answered
# by the same events, a cancellation that leaves no process, and an exec
# whose exit the kernel records as the designated init's.
run-etudeguest: rach2 test-image utils $(ETUDESERIAL)
	$(ETUDESERIAL) --kernel $(BOOT_KERNEL) --initrd $(TEST_IMAGE) \
	    --hello $(ARCH_BUILD)/tests/hello --ish $(ARCH_BUILD)/ish/ish \
	    --yes $(ARCH_BUILD)/utils/yes --log $(IMAGES_DIR)/etudeguest-test.log
# `tests/run etudeexec`: Rach2 builds etude by its TinyCC recipe, and the POSIX
# executor, built the same way, passes its test there. Boots os-real from a
# disk with a second disk that is a tar archive of etude's source and the
# executor's test, and requires a built and then a reused version, the built
# casecheck and history_check to pass, and every row of the executor's test,
# the subreaper's among them, to run and pass. ETUDEEXEC_TREE, empty but in
# tests/system/etudeexec_mutate.sh, names a copy of the files to archive.
ETUDEEXEC_TEST_DISK = $(IMAGES_DIR)/etudeexec-test.disk
ETUDEEXEC_TREE =
run-etudeexec: $(DEMO_CREDS) rach2 krffs $(MANIFEST_PARTS_os_real)
	@mkdir -p $(IMAGES_DIR)
	$(MKDISK) krffs/manifests/os-real.manifest $(ETUDEEXEC_TEST_DISK) 64 48
	sh tests/system/suites/etudeexec.sh $(BOOT_KERNEL) $(ETUDEEXEC_TEST_DISK) \
	    "`cat krffs/rach2_demo_password`" "$(ETUDEEXEC_TREE)"
# Everything every suite needs, in one target, so `tests/run all` can build once
# through one Make and then run the suites at the same time. Two suites each
# invoking their own build would be two Makes against one `build/` tree; with
# this built first, each suite's own target finds its work done and only boots
# machines. The list is the union of what the `run-*` targets depend on, and
# `tests/run` checks that it stays the union.
#
# The components, the builds that turn sources into programs, are a target of
# their own, built in a Make of their own before the one that builds images
# and disks: `prepare-all` builds them and then runs a second Make for the
# rest, and `tests/run` builds them before a lone suite's own target. An
# image rule also orders each staged component file after its builder, so a
# single make restages a rebuilt program. The runner keeps preparation in two
# invocations so every component finishes before suite products are built.
components: harness-kernel busybox check-busybox check-dropbearmulti check-libc \
            check-pdpmake check-setid check-tcc ish krffs musl-programs \
            ports/doom rach2 rach2-metal rach2-recovery tests tools/etude/guest utils

prepare-images: $(DEMO_CREDS) $(BINFIND) $(CASECHECK) $(DEADLINE) $(ELFCHECK) $(ETUDEHOST) $(ETUDELEGACY) \
                $(ETUDELINES) $(ETUDESERIAL) $(HARNESS) \
                $(HOSTPEER) $(MKFIXTURE) $(MKGPT) $(MKMBR) $(PCAPCHECK) $(PTYDRIVE) $(RUNTESTS) \
                $(RUNUTIL) $(SCREENTEXT) $(SHA256TOOL) $(WAVCHECK) $(GROWSET) os-real-image \
                os-simple-image rach2-metal-image rach2-recovery-image $(SEALED_DISK) test-image \
                $(INSTALL_STICK) $(INSTALL_TEST_STICK) $(INSTALLER_MBR_DISK) $(GROW_GPT_DISK)
prepare-all: components
	$(MAKE) --no-print-directory prepare-images prepare-products
# The commands beyond the build's that the preparation runs, in the registry's
# form: xorriso for the ISOs and the sticks, and mtools for the GRUB disks.
# `tests/run` reads them from this target and refuses a set on a host that
# lacks one before it builds anything, since a set prepares every suite's
# products whichever suites it names.
PREPARE_TOOLS = xorriso,mformat,mmd,mcopy
prepare-tools:
	@echo $(PREPARE_TOOLS)
# `tests/run addressed`: the address named on the boot line, and the proof that
# it is the one in force. Its case is in tests/cases/machine.addressed.json,
# which etude runs, and has no target here.
# `tests/run noentropy`: the entropy policy's fail-closed half, which no other
# suite can reach, on a processor without RDRAND and RDSEED. The kernel's
# generator is untrusted there: the salt mint refuses and writes nothing,
# passwd refuses before it asks for a new password, the random devices and
# getrandom answer EIO while GRND_INSECURE still returns bytes, and
# Dropbear's key generator exits with its error and writes no key, which is
# why rach2/run.sh and its siblings boot -cpu max. They are the cases of
# tests/cases/guest.noentropy.json, which etude runs, and have no target here.
# `tests/run fpu`: the floating-point state of a process is its own, by each
# path the kernel saves it with: XSAVE under QEMU's `max`, FXSAVE under
# `rach2.noxsave` and on `qemu64`, where `xsaveprobe` must find OSXSAVE clear
# and no NT_X86_XSTATE, and the x87 state alone by `fnsave` under
# `rach2.nofxsr`, which only the i686 kernel takes, since every x86-64
# processor has FXSR. They are the cases of tests/cases/guest.fpu.json, which
# etude runs, and have no target here.
# `tests/run bigmem`: all of an x86-64 machine's eight gigabytes is counted,
# and a program's pages fill memory past what lies below 4 GiB and read back
# as written; without 1 GiB pages, 72 GiB is counted to the direct map's
# 64 GiB. They are the machine cases of tests/cases/machine.bigmem.json,
# which etude runs, and have no target here. The 72 GiB case boots only where
# the site's policy has the line `boundary_lazy bigmem-boundary-v1`, and
# elsewhere ends incomplete before its machine starts.
# `tests/run memory`: the kernel's account of physical memory at five sizes,
# memprobe and frame allocations failed at the two ends of the range, the
# string routines swept, a machine of 8 MiB refused and kernel stacks run
# out. They are the machine cases of tests/cases/machine.memory.json, which
# etude runs, and have no target here. The case of 8 MiB boots the recovery
# kernel with this copy of its image, whose name does not depend on whether
# the image carries a key.
$(IMAGES_DIR)/memory-test.recovery.krffs: $(RECOVERY_IMAGE)
	cp $(RECOVERY_IMAGE) $@
# `tests/run pools`: many pipes, ptys, sockets and processes live at once,
# and the kernel heap back where it started after a thousand each of pipes,
# ptys and sockets (see tests/poolprobe.c). They are the guest cases of
# tests/cases/guest.pools.json, which etude runs, each in a boot of its own,
# and have no target here.
# `tests/run fault`: the three probes whose whole claim is that the kernel
# killed them. vmfault writes through a pointer in no mapping, vmro writes a
# page it mapped read-only, and winprobe reads the page-cache window, which
# belongs to the kernel. They are the cases of tests/cases/guest.fault.json,
# which etude runs, each in a boot of its own, and have no target here: each
# must end on signal 11 with the kernel's record of the kill on the console
# and no panic.
# `tests/run console`: the two probes that read from the console, which the
# shared probe boot cannot run because they block until something is typed.
# They are the cases of tests/cases/machine.console.json, which etude runs,
# each in a boot of its own, and have no target here: stdintest reads three
# lines and prints what it read, so the case judges the text as well as the
# count, and the third read asks for five bytes of a longer line; ttyprobe
# takes a cooked line with two erases in it, a raw byte with no newline, the
# screen's answers to its queries, and Alt with x pressed on the machine's
# keyboard.
# `tests/run audio`: what the machine actually played. Its cases
# (tests/cases/machine.audio.json) play the debug shell's scale on an Intel
# HDA controller and on a Sound Blaster 16, and wavcheck measures the notes in
# QEMU's recording of each.
# `tests/run diskfull`: a file system with no room left. The probe writes until
# the write is refused, requires `ENOSPC` rather than `EIO`, removes the file
# and writes again. It is the case of tests/cases/guest.diskfull.json, which
# etude runs in a boot of its own, since it fills the test image's root, and
# has no target here.
# `tests/run socket`: the guest opening connections of its own, checked by a
# host that answers (see tests/cases/machine.socket.json). They are the cases
# of that manifest, which etude runs, and have no target here.
# `tests/run disk`: the disk-backed page-cache integrity check. It is the case
# of tests/cases/guest.disk.json, which etude runs, and has no target here:
# blockprobe on a KRFFS root that rach2.fsdisk mounts from a disk on IDE.
# blockprobe's 6 MiB fit in the 8 MiB cache of this machine, so it proves that
# data written and fsynced through the cache reads back whole; the stress
# suite's 33 MiB exercise eviction.
# `tests/run bootpart`: /proc/cmdline and the level-one disk devices. They
# are the four cases of tests/cases/machine.bootpart.json, which etude runs,
# and have no target here, each a boot that runs bootprobe at the debug
# shell. On the disk as mkdisk.sh made it, with a single KRFFS partition,
# /dev/bootpart must be absent from stat and open. On the boot fixture, which
# adds a FAT-typed partition of known bytes, the probe drives the device end
# to end (the 0600 mode from both stat paths, seeks against the partition
# size, known bytes at a seek, a written sector read back, exactly 0 at the
# end, -ENOSPC past it, -EINVAL unaligned, -EACCES unprivileged). A third
# boot mounts a RAM image instead of the disk and requires both /dev/bootpart
# and /dev/grubcore anyway, since a machine that mounts nothing from its disk
# must still manage its boot partition and its boot loader, which is what
# recovery depends on. A fourth attaches a sparse 260 GiB drive over AHCI and
# reads known sectors at 5 GiB and at 258 GiB, which a 32-bit file offset
# could not address. It is that large on purpose: a few gigabytes would be
# satisfied by an implementation that kept 33 bits, and it is attached over
# AHCI because the ATA driver's 28-bit LBA stops at 128 GiB.
FD_HOSTTEST = build/tests/host/fd_hosttest
FD_HOSTTEST_SOURCES = tests/host/fd_hosttest.c rach2/sys/fs/fd.c \
                     rach2/sys/lib/kstring.c rach2/sys/lib/kctypes.c
$(FD_HOSTTEST): $(FD_HOSTTEST_SOURCES) rach2/sys/proc/process.c
	@mkdir -p $(dir $@)
	$(HOSTCC) -std=c99 -DRACH2_HOSTTEST -g -O1 -ffreestanding -Wno-empty-body \
	    -I rach2/sys -Wno-pointer-to-int-cast -Wno-int-to-pointer-cast \
	    -fsanitize=address,undefined -fno-sanitize-recover=all \
	    -o $@ $(FD_HOSTTEST_SOURCES)
	$(HOSTCC) -DRACH2_HOSTTEST -I rach2/sys -MM -MP -MT $@ $(FD_HOSTTEST_SOURCES) > $@.d
-include $(FD_HOSTTEST).d

# `tests/run fdlock`: pending KRFFS relocation, orphan truncation, and
# concurrent raw offsets. The raw boot is under the lag, so its disk keeps
# the guest's flushes from the host (cache=unsafe), as tests/system/Readme.md
# says.
# Its cases are tests/cases/guest.fdlock.json, with no target here.
# The disks of the bootpart cases, tests/cases/machine.bootpart.json, made for
# each architecture, since their root holds tests/bootprobe: the disk that
# mkdisk.sh makes of disk-test.manifest, with one KRFFS partition, and that
# disk after `mkfixture boot` has given it a FAT-typed entry 1 of known bytes.
# The wide case's disk of 260 GiB is an empty disk of its machine, into which
# the scenario has mkfixture write the markers.
BOOTPART_ABSENT_DISK = $(ARCH_CASE_DIR)/bootpart/absent.disk
BOOTPART_FIXTURE_DISK = $(ARCH_CASE_DIR)/bootpart/fixture.disk
$(BOOTPART_ABSENT_DISK): krffs/manifests/disk-test.manifest $(MANIFEST_DEPS_disk_test) $(MKDISK) \
                         build/krffs/mkfs.krffs build/krffs/stageimage.krffs \
                         | preflight krffs tests
	@mkdir -p $(@D)
	rm -f $@.tmp
	$(MKDISK) krffs/manifests/disk-test.manifest $@.tmp 64 48
	mv $@.tmp $@
$(BOOTPART_FIXTURE_DISK): $(BOOTPART_ABSENT_DISK) $(MKFIXTURE)
	cp $(BOOTPART_ABSENT_DISK) $@.tmp
	$(MKFIXTURE) boot $@.tmp || { rm -f $@.tmp; exit 1; }
	mv $@.tmp $@
# `tests/run grub`: the machine replacing its own boot loader. It is the case
# of tests/cases/machine.grub.json, which etude runs, and has no target here.
# The gap after the MBR has two core-image tails and LBA 1 selects between
# them, so a replacement writes the tail not in use, reads it back, and only
# then rewrites that one sector; sector 0, which has the partition table, is
# never written. The case's three boots of one GRUB disk install the two
# marked cores in turn, and every boot's marker must be the core the boot
# before it installed. The marker comes from inside the running core, because
# reading the pointer back would prove only bytes at rest.
#
# The disk's root is an ext2 root made from grub-test.manifest, which has what
# the commands the case types expect, and the bootorder suite boots it too.
# The two marked cores the root's manifest stages. They have rules of their
# own, and the root waits for them, so that the root stages the pair this
# build made.
GRUB_CORE_A = $(IMAGES_DIR)/core-a.img
GRUB_CORE_B = $(IMAGES_DIR)/core-b.img
# The pinned core and the manifest it is verified against are inputs of these
# fixtures, not just the script that copies them: a regenerated core with the
# script unchanged would otherwise leave the marked pair as it was.
GRUB_CORE_DEPS = tools/image/mkgrubcore.sh vendor/grub/verify_grub.sh \
                 vendor/grub/manifest.txt
$(GRUB_CORE_A): $(GRUB_CORE_DEPS) vendor/grub/disk-core-a.img
	@mkdir -p $(IMAGES_DIR)
	sh tools/image/mkgrubcore.sh A $@
$(GRUB_CORE_B): $(GRUB_CORE_DEPS) vendor/grub/disk-core-b.img
	@mkdir -p $(IMAGES_DIR)
	sh tools/image/mkgrubcore.sh B $@

GRUB_TEST_EXT2 = $(IMAGES_DIR)/grub-test.ext2
$(GRUB_TEST_EXT2): $(DEMO_CREDS) $(GRUB_CORE_A) $(GRUB_CORE_B) krffs/manifests/grub-test.manifest $(MANIFEST_DEPS_grub_test) tools/image/mkext2.sh $(IMAGE_TOOLS) | preflight krffs rach2 $(MANIFEST_PARTS_os_real)
	sh tools/image/mkext2.sh krffs/manifests/grub-test.manifest $@ 48
# The disk of the grub case, tests/cases/machine.grub.json: the GRUB disk
# around the kernel, on an MBR, with the grub-test root. The recipe first
# requires grub-test.manifest, less its comments and its two core lines, to
# be os-real.manifest, so that the case's preparation stops on a root that
# has drifted from os-real before any machine boots it; a host case of the
# family would report the drift only after the disk had been made from it.
GRUB_CASE_DISK = $(ARCH_CASE_DIR)/grub/grub.disk
$(GRUB_CASE_DISK): $(DEMO_CREDS) tools/image/mkdisk_grub.sh $(KERNEL) $(GRUB_TEST_EXT2) \
                   krffs/manifests/grub-test.manifest krffs/manifests/os-real.manifest \
                   $(DISK_TOOLS) $(GRUB_FILES) | rach2 krffs $(MANIFEST_PARTS_os_real)
	@mkdir -p $(@D)
	sed -e '/^#/d' -e '/^$$/d' -e '\|^file 0644 /core-[ab]\.img|d' \
	    krffs/manifests/grub-test.manifest > $@.manifest
	sed -e '/^#/d' -e '/^$$/d' krffs/manifests/os-real.manifest | diff - $@.manifest || \
	    { echo "grub: grub-test.manifest differs from os-real.manifest beyond its two cores" >&2; \
	      rm -f $@.manifest; exit 1; }
	rm -f $@.manifest
	RACH2_DISK_TABLE=mbr \
	RACH2_DISK_EXT2_MB=48 \
	RACH2_DISK_EXT2_IMAGE=$(GRUB_TEST_EXT2) \
	RACH2_DISK_RECOVERY_MB=8 \
	    sh tools/image/mkdisk_grub.sh $(KERNEL) $@
# `tests/run bootorder`: the GRUB disk on the second AHCI port behind a
# disk holding nothing, booted by the firmware, its root found as disk 1. It
# is the case of tests/cases/machine.bootorder.json, which etude runs, and has
# no target here.
# The GRUB disk of its case, which the case's preparation names: the GRUB
# disk around the kernel, on an MBR, its root the grub-test tree as an ext2
# partition, which mkdisk_grub.sh requires of every disk, and rach2.disk=ahci
# on each entry's line. The blank disk in front of it is an empty disk of the
# case.
BOOTORDER_CASE_DISK = $(ARCH_CASE_DIR)/bootorder/bootorder.disk
$(BOOTORDER_CASE_DISK): $(DEMO_CREDS) tools/image/mkdisk_grub.sh tools/image/grub-embed.cfg \
                        $(KERNEL) $(GRUB_TEST_EXT2) $(DISK_TOOLS) $(GRUB_FILES) \
                        | rach2 krffs $(MANIFEST_PARTS_os_real)
	@mkdir -p $(@D)
	RACH2_DISK_TABLE=mbr \
	RACH2_DISK_APPEND="rach2.disk=ahci" \
	RACH2_DISK_RECOVERY_MB=8 \
	RACH2_DISK_EXT2_MB=48 \
	RACH2_DISK_EXT2_IMAGE=$(GRUB_TEST_EXT2) \
	    sh tools/image/mkdisk_grub.sh $(KERNEL) $@
# `tests/run menu`: the boot menu that can be rewritten, and the stub that
# cannot. It is the case of tests/cases/machine.menu.json, which etude runs,
# and has no target here: four boots of one GRUB disk, a two-entry menu staged
# through `bootctl config` with a grubenv key round-tripped and the candidate
# kernel destroyed; a boot proving both staged entries reached the menu and
# that the fallback number still named the stub's own entry rather than a
# staged one; that file torn mid-entry from the host, after which the machine
# must still boot through the stub with GRUB's parse error in the log; and the
# slot's extent read back to prove bootctl overwrote bytes in place rather
# than rewriting a file.
# `tests/run authority`: which kernel may write the whole disk. The
# authority is read off each built kernel, and then each boots the test image
# with a disk over AHCI: the everyday build must refuse a write to /dev/disk0
# with -EPERM and the sector must still be zero afterwards, so the refusal is
# proved to have written nothing rather than merely returned an error; the
# recovery build must write a sector past 2 GiB, read it back byte for byte,
# and take a 200-sector write in one call, which is what makes a whole-disk
# reflash finishable: one sector per driver call would turn a 500 MiB write
# into a million cache flushes. They are the cases of
# tests/cases/guest.authority.json, which etude runs, and have no target here.
#
# The disk of the boots, which the cases' preparation names: 4 GiB of zeros
# with an MBR signature, made beside its name and renamed into place.
$(IMAGES_DIR)/auth-test.disk: $(MKFIXTURE)
	@mkdir -p $(@D)
	$(MKFIXTURE) auth $@.tmp && mv $@.tmp $@
# `tests/run install`: the machine installs its own kernel. It is the case of
# tests/cases/machine.install.json, which etude runs, and has no target here:
# on the GRUB disk below, booted with no -kernel, the whole install loop twice
# as root, a marker edited into the banner source, the kernel built in place,
# installed as a one-boot trial, proved running by its marker after
# reboot -f, committed, held across a cold boot, corrupted at the extent
# bootctl names, recovered through the fallback and repaired from inside; then
# the interruption matrix's determinate states, booted from environment
# blocks that the case's scenario writes into its copy of the disk.
# The disk's entries boot the ext2 root, the os-real system at 96 MiB. The
# kernel is built on that root and installed from it.
OS_REAL_SMALL_EXT2 = $(IMAGES_DIR)/os-real-small.ext2
$(OS_REAL_SMALL_EXT2): $(DEMO_CREDS) krffs/manifests/os-real.manifest $(MANIFEST_DEPS_os_real) tools/image/mkext2.sh $(IMAGE_TOOLS) | preflight krffs rach2 $(MANIFEST_PARTS_os_real)
	sh tools/image/mkext2.sh krffs/manifests/os-real.manifest $@ 96
# The disk of the menu case, tests/cases/machine.menu.json: the GRUB disk
# around the kernel, on an MBR, with os-real's root, since every line the
# case types at the machine is a BusyBox command.
MENU_DISK = $(ARCH_CASE_DIR)/menu/menu.disk
$(MENU_DISK): $(DEMO_CREDS) tools/image/mkdisk_grub.sh $(KERNEL) $(OS_REAL_SMALL_EXT2) $(DISK_TOOLS) \
              $(GRUB_FILES) | rach2 krffs $(MANIFEST_PARTS_os_real)
	@mkdir -p $(@D)
	RACH2_DISK_TABLE=mbr \
	RACH2_DISK_RECOVERY_MB=8 \
	RACH2_DISK_EXT2_MB=96 \
	RACH2_DISK_EXT2_IMAGE=$(OS_REAL_SMALL_EXT2) \
	    sh tools/image/mkdisk_grub.sh $(KERNEL) $@
# The disk of tests/cases/machine.install.json, the GRUB disk around the
# architecture's kernel on an MBR, with the recovery kernel and its image on
# partition 2 and the os-real root above on partition 3.
$(ARCH_CASE_DIR)/install.disk: $(KERNEL) $(RECOVERY_KERNEL) $(RECOVERY_IMAGE) $(OS_REAL_SMALL_EXT2) \
                               tools/image/mkdisk_grub.sh $(DISK_TOOLS) $(GRUB_FILES) | rach2
	@mkdir -p $(@D)
	RACH2_DISK_TABLE=mbr \
	RACH2_DISK_RECOVERY_MB=8 \
	RACH2_DISK_EXT2_MB=96 \
	RACH2_DISK_EXT2_IMAGE=$(OS_REAL_SMALL_EXT2) \
	RACH2_DISK_RECOVERY_KERNEL=$(RECOVERY_KERNEL) \
	RACH2_DISK_RECOVERY_IMAGE=$(RECOVERY_IMAGE) \
	    sh tools/image/mkdisk_grub.sh $(KERNEL) $@
# `tests/run sealed`: the disk that goes into the laptop, operated the way it
# will be operated once it is in there. It is the two cases of
# tests/cases/machine.sealed.json, which etude runs on separate copies of the
# one disk, and has no target here: a fault case that walks the whole life of
# the machine over SSH (a kernel sent down the connection, installed, tried and
# committed; a destroyed candidate caught by the fallback and repaired; a
# missing, a damaged and an unclean root each landing in recovery and repaired;
# a menu staged and then torn; the boot loader replaced through /dev/grubcore;
# the root destroyed and rewritten from recovery), and a control case that
# injects nothing and finds no fallback and no recovery anywhere.
# The artifact of the sealed cases beyond the kernel, the sealed disk and the
# demo key: GRUB's pinned GPT core, which the fault case installs through
# /dev/grubcore, checked against the GRUB manifest as it is copied.
$(CASE_DIR)/sealed/gpt-core.img: vendor/grub/gpt-core.img vendor/grub/manifest.txt \
                                 vendor/grub/verify_grub.sh $(SHA256TOOL)
	@mkdir -p $(@D)
	cp vendor/grub/gpt-core.img $@.new
	sh vendor/grub/verify_grub.sh "gpt-core.img=$@.new" || { rm -f $@.new; exit 1; }
	mv $@.new $@
# `tests/run drill`: the recovery drill, boots of one disk that carries the same
# three partitions the sealed disk has. It is the cases of
# tests/cases/machine.drill.json, which etude runs, and has no target here: the
# everyday kernel refuses to write its own disk and is shown to have written
# nothing; a root destroyed with the machine stopped stops the boot; the
# recovery entry comes up from partition 2, takes an address and answers SSH
# with nothing typed at the console; the whole root partition arrives through
# one `ssh` pipe and the disk then has the image's bytes; the machine boots from
# it; a recovery payload that will not load falls back to partition 1; recovery
# on a network with no DHCP server takes its fixed address; and a disk whose
# boot partition is destroyed still reaches recovery through the core image's
# own configuration. The disk is small so the drill is quick.
#
# Its entries have `rach2.diskdiag`, which is what publishes /dev/disk0 on an
# everyday kernel: the refusal is worth proving on a machine that has the device
# rather than on one where the node is simply absent. The sealed disk has no
# such flag, so there the everyday entries have no whole-disk node at all, and
# the recovery build publishes its own whatever the command line says. They
# also have `rach2.ip=none`, so that recovery takes its address from a DHCP
# server or, with none, its fixed one.
# The os-simple system as an ext2 volume of 16 MiB: the drill disk's root, which
# the everyday kernel must refuse to write, that is destroyed, that recovery
# rewrites through the pipe, and that a later boot mounts again. The parts and
# selfbuild suites use it too.
OS_SIMPLE_EXT2 = $(IMAGES_DIR)/os-simple.ext2
$(OS_SIMPLE_EXT2): $(DEMO_CREDS) krffs/manifests/os-simple.manifest $(MANIFEST_DEPS_os_simple) tools/image/mkext2.sh $(IMAGE_TOOLS) | preflight krffs rach2 utils check-setid
	sh tools/image/mkext2.sh krffs/manifests/os-simple.manifest $@ 16
DRILL_CASE_DISK = $(ARCH_CASE_DIR)/drill/drill.disk
$(DRILL_CASE_DISK): $(DEMO_CREDS) tools/image/mkdisk_grub.sh tools/image/grub-embed.cfg $(KERNEL) \
                    $(RECOVERY_KERNEL) $(RECOVERY_IMAGE) $(OS_SIMPLE_EXT2) $(DISK_TOOLS) $(GRUB_FILES)
	@mkdir -p $(@D)
	RACH2_DISK_TABLE=mbr \
	RACH2_DISK_RECOVERY_MB=8 \
	RACH2_DISK_EXT2_MB=16 \
	RACH2_DISK_EXT2_IMAGE=$(OS_SIMPLE_EXT2) \
	RACH2_DISK_APPEND="rach2.diskdiag" \
	RACH2_DISK_DHCP_APPEND="rach2.ip=none" \
	RACH2_DISK_RECOVERY_KERNEL=$(RECOVERY_KERNEL) \
	RACH2_DISK_RECOVERY_IMAGE=$(RECOVERY_IMAGE) \
	    sh tools/image/mkdisk_grub.sh $(KERNEL) $@
# `tests/run persist`: fsync durability across a power cut. It is the case of
# tests/cases/machine.persist.json, which etude runs, and has no target here:
# two boots of one private copy of this disk on IDE, the first writing and
# fsyncing /persist.txt and then cut off by QMP's quit, the second reading it
# back. The disk is the persist-ahci case's too.
PERSIST_DISK = $(IMAGES_DIR)/persist-test.disk
$(PERSIST_DISK): krffs/manifests/persist-test.manifest $(MANIFEST_DEPS_persist_test) $(MKDISK) \
                 $(IMAGE_TOOLS) | preflight krffs tests
	$(MKDISK) krffs/manifests/persist-test.manifest $@ 64 48
# `tests/run cred-persist`: the credential replacement's durability across a
# reboot. Both of cred_rewrite's flushes are no-ops on the RAM module the probe
# suite boots, so a library that dropped either would pass every other probe.
# This boots one disk image three times. The first boot publishes one
# replacement and releases, then publishes a second and is cut off holding the
# lock, as a power failure would cut it off. The second boot reads both back
# with the modes and owners they inherited, and reports the lock the cut-off
# writer left rather than breaking it. The third reads back the release itself:
# a lock whose removal stayed in the page cache would still be there.
# Its cases are tests/cases/machine.cred-persist.json, with no target here.
CRED_PERSIST_DISK = $(IMAGES_DIR)/cred-persist-test.disk
# `tests/run crash`: prove the dirty-mount flag detects an unclean shutdown.
# Its cases (tests/cases/machine.crash.json) boot copies of this disk and read
# the flag with fsck.krffs. Its own image, so that it cannot race
# another suite on a shared file under `make -j`.
CRASH_TEST_IMAGE = $(IMAGES_DIR)/crash-test.disk
$(CRASH_TEST_IMAGE): krffs/manifests/disk-test.manifest $(MANIFEST_DEPS_disk_test) $(MKDISK) \
                     build/krffs/mkfs.krffs build/krffs/stageimage.krffs | preflight krffs tests
	$(MKDISK) krffs/manifests/disk-test.manifest $@ 64 48
# `tests/run ping`: the ICMP path against the SLIRP gateway, which is why it is
# a case of its own rather than a probe in the default suite. Its case is in
# tests/cases/guest.ping.json, which etude runs, and has no target here.
# `tests/run packet`: the packet socket, which is how a DHCP client sees a reply
# that arrives before this machine owns an address. Its case is in
# tests/cases/guest.packet.json, which etude runs, and has no target here.
# `tests/run bootline`: the boot line that contradicts itself. `rach2.nodisk`
# skips the disk an image rooted on it needs, and the machine would fall to a
# shell that authenticates nobody, so the kernel refuses that combination; with
# `rach2.rescue` it is allowed, since asking for that shell is what the token is
# for. A disk root with nothing on the disk, a root that mounted and whose
# /sbin/init cannot be read, a symbolic link to itself on an ext2 volume, and a
# processor without what the kernel needs are refused the same way. Each
# refusal is asserted twice: the diagnostic, and the machine halted with no
# shell, or for a processor no kernel code, after it. A boot line of 248 bytes
# after the kernel's path, which QEMU's loader puts first, is kept whole, and
# one of 668 is cut at a word and logged as cut. They are the cases of
# tests/cases/machine.bootline.json, which etude runs, each in a boot of its
# own, and have no target here.
# The ext2 root of the bootline case whose /sbin/init is a link to itself.
BOOTLINE_LOOP_DISK = $(ARCH_CASE_DIR)/bootline/loop.disk
$(BOOTLINE_LOOP_DISK): tests/system/mkrootdisk.sh tools/setup/e2fsprogs_path.sh $(MKMBR)
	@mkdir -p $(@D)
	sh tests/system/mkrootdisk.sh loop $@ $(MKMBR)
# `tests/run servicelog`: the log init's services write to is bounded within a
# boot, and a log a boot left full is removed at the next. An ext2 root with
# init-shell as its init and BusyBox as its shell, booted twice: the first
# fills the log past its cap and leaves orphans to init until it is emptied,
# fills it again and powers off; the second finds it removed. They are the
# cases of tests/cases/machine.servicelog.json, which etude runs, and have no
# target here.
SERVICELOG_ROOT_DISK = $(ARCH_CASE_DIR)/servicelog/root.disk
$(SERVICELOG_ROOT_DISK): tests/system/mkrootdisk.sh tools/setup/e2fsprogs_path.sh $(MKMBR) \
                         $(ARCH_BUILD)/utils/sbin/init-shell $(ARCH_BUILD)/utils/update \
                         $(BUSYBOX_STAMP) | preflight utils check-busybox
	@mkdir -p $(@D)
	sh tests/system/mkrootdisk.sh servicelog $@ $(MKMBR) $(ARCH_BUILD)/utils/sbin/init-shell \
	    $(ARCH_BUILD)/utils/update $(BUSYBOX_BIN)
# `tests/run raw`: the raw ICMP socket, which is what `ping` and `traceroute`
# are. Its cases, each in a boot of its own, are in
# tests/cases/machine.raw.json, which etude runs, and have no target here.
# `tests/run netif`: the interface ioctls, which is how a program sets this
# machine's address, and machines with two cards. Its cases are in
# tests/cases/machine.netif.json, which etude runs, and have no target here.
# `tests/run dgram`: a connected datagram socket and the permission to
# broadcast, which is how a DHCP client renews a lease and how it asks for one.
# Its case is in tests/cases/machine.dgram.json, which etude runs, and has no
# target here.
# `tests/run dhcp`: the machine taking an address it was not told, with both of
# its clients: BusyBox's `udhcpc`, which `ifup` runs, and os-simple's own
# `dhcpc`, which its `netup` runs. Its cases are in
# tests/cases/machine.dhcp.json, which etude runs, and have no target here.
# `tests/run netconsole`: the kernel log on the wire, the cases of
# tests/cases/machine.netconsole.json.
# `tests/run e1000`: the network path on the other emulated card, and the probe
# order that decides which card a machine with more than one of them gets, the
# cases of tests/cases/machine.e1000.json, each a boot of its own.
#
# This card is the one QEMU attaches when nobody asks for one, so it is what a
# person running qemu-system-i386 from a generic command line has. The suites
# run on it as well are the ones whose subject is the wire: ICMP, the packet
# socket, the interface ioctls, broadcast, netconsole, the raw socket, DHCP,
# and SSH, which uses the TCP path most. Each is a registry entry of its own,
# `NAME-e1000`, so that the pool schedules it beside its first-card twin and
# can see what it costs. The other suites are about disks, accounts and
# building, and running them again on a second card would cost minutes and
# show nothing.
# `tests/run reset`: each reset mechanism on its own. The kernel tries three,
# most of the machine first, and each case of tests/cases/machine.reset.json
# boots the kernel and the test image with the ones before it left out, so
# that each must still restart the machine.
# `tests/run diskdiag`: a disk read without being mounted and without being
# written. It is the case of tests/cases/machine.diskdiag.json, which etude
# runs, and has no target here: the ordinary kernel mounting a copy of the
# disk, which must change it; the read-only kernel reading another copy over
# SSH with nothing typed at the console, where every sector hashed on the
# machine must equal the same sector hashed on the host; and the same kernel
# with no drive at all.
# `tests/run subset`: each optional device probe left out, and proved left
# out. They are the cases of tests/cases/guest.subset.json, which etude runs,
# and have no target here.
# `tests/run usb`: the USB stack, from a port to a keystroke, the cases of
# tests/cases/machine.usb.json, each a boot of its own.
# `tests/run mount`: the partition nodes, the block path through a device
# that sleeps or never answers, mount(2) and the claims it makes, and the FAT
# driver mounted from BusyBox's shell. It is the cases of
# tests/cases/machine.mount.json, which etude runs, and has no target here.
# The disks of the mount cases are made for each architecture, since the
# FAT32 volume holds its tests/hello and tests/memprobe. MOUNT_TEST_DISK is
# the test disk with a 48 MiB KRFFS partition at LBA 2048 and a 14 MiB FAT32
# one at LBA 100352, whose volume mtools fills as a used one is: a name
# written, deleted and written again.
# mformat's -T makes the volume the partition's 28672 sectors, since the
# driver refuses a volume that claims more sectors than its partition has.
# pattern.bin is 128 sectors, each 128 copies of its own number and a dot.
# MOUNT_TEST_LIST and MOUNT_TEST_SUMS are what mtools reads off that volume:
# its names sorted on one line, and the cksum of three of its files, which
# the busybox case compares with what the machine's shell printed.
# MOUNT_BOOT_DISK is the boot fixture of `mkfixture boot`, for the write
# through /dev/bootpart, and MOUNT_CLAIM_DISK the mount disk with its MBR
# entries swapped, so that the boot partition is the FAT32 volume.
MOUNT_TEST_DISK  = $(IMAGES_DIR)/mount-test.disk
MOUNT_TEST_LIST  = $(IMAGES_DIR)/mount-test.list
MOUNT_TEST_SUMS  = $(IMAGES_DIR)/mount-test.sums
MOUNT_BOOT_DISK  = $(IMAGES_DIR)/mount-test-boot.disk
MOUNT_CLAIM_DISK = $(IMAGES_DIR)/mount-test-claim.disk
MOUNT_FAT        = @@51380224
$(MOUNT_TEST_DISK): krffs/manifests/disk-test.manifest $(MANIFEST_DEPS_disk_test) $(MKDISK) \
                    build/krffs/mkfs.krffs build/krffs/stageimage.krffs $(MKMBR) \
                    $(wildcard $(ARCH_BUILD)/tests/hello $(ARCH_BUILD)/tests/memprobe) \
                    | preflight krffs tests
	rm -f $@.tmp $@.hello $@.pattern
	$(MKDISK) krffs/manifests/disk-test.manifest $@.tmp 64 48
	$(MKMBR) --lba $@.tmp 2048:98304:7f 100352:28672:0c
	mformat -i $@.tmp$(MOUNT_FAT) -F -T 28672 -v M36FAT ::
	echo "hello from the mount suite" > $@.hello
	mcopy -i $@.tmp$(MOUNT_FAT) $@.hello ::/hello.txt
	mcopy -i $@.tmp$(MOUNT_FAT) $(ARCH_BUILD)/tests/hello ::/hello
	mcopy -i $@.tmp$(MOUNT_FAT) $@.hello ::/gone.txt
	mdel -i $@.tmp$(MOUNT_FAT) ::/gone.txt
	mcopy -i $@.tmp$(MOUNT_FAT) $(ARCH_BUILD)/tests/hello ::/again.bin
	awk 'BEGIN { for (i = 0; i < 128; i++) for (j = 0; j < 128; j++) printf "%03d.", i }' \
	    > $@.pattern
	mcopy -i $@.tmp$(MOUNT_FAT) $@.pattern ::/pattern.bin
	mcopy -i $@.tmp$(MOUNT_FAT) $(ARCH_BUILD)/tests/memprobe ::/memprobe
	rm -f $@.hello $@.pattern
	mv $@.tmp $@
$(MOUNT_TEST_LIST): $(MOUNT_TEST_DISK)
	mdir -i $(MOUNT_TEST_DISK)$(MOUNT_FAT) -b :: > $@.tmp
	sed 's#::/##' $@.tmp | LC_ALL=C sort | tr '\n' ' ' | sed 's/ *$$//' > $@.list
	rm -f $@.tmp
	mv $@.list $@
$(MOUNT_TEST_SUMS): $(MOUNT_TEST_DISK)
	rm -f $@.tmp
	for name in hello again.bin hello.txt; do \
	    mcopy -i $(MOUNT_TEST_DISK)$(MOUNT_FAT) ::/$$name - > $@.file || exit 1; \
	    cksum < $@.file | awk -v name=$$name '{ print $$1, $$2, name }' >> $@.tmp || exit 1; \
	done
	rm -f $@.file
	mv $@.tmp $@
$(MOUNT_BOOT_DISK): krffs/manifests/disk-test.manifest $(MANIFEST_DEPS_disk_test) $(MKDISK) \
                    build/krffs/mkfs.krffs build/krffs/stageimage.krffs $(MKFIXTURE) \
                    | preflight krffs tests
	rm -f $@.tmp
	$(MKDISK) krffs/manifests/disk-test.manifest $@.tmp 64 48
	$(MKFIXTURE) boot $@.tmp
	mv $@.tmp $@
$(MOUNT_CLAIM_DISK): $(MOUNT_TEST_DISK) $(MKMBR)
	cp $(MOUNT_TEST_DISK) $@.tmp
	$(MKMBR) --lba $@.tmp 100352:28672:0c 2048:98304:7f
	mv $@.tmp $@

# `tests/run load`: the machine under load while six lines are typed at its
# console: every line read, no request quarantined by its deadline, and the
# longest delays it saw, from `/proc/stat`, printed. It is the cases of
# tests/cases/machine.load.json, which etude runs, and has no target here:
# the load on a disk behind the lag device whose flushes QEMU keeps from the
# host, and the polled writer of `rach2.atapio`. Their disk is 16 MiB with a
# FAT32 partition at LBA 2048.
LOAD_DISK = $(IMAGES_DIR)/load-test.disk
$(LOAD_DISK): $(MKMBR)
	@mkdir -p $(@D)
	dd if=/dev/zero of=$@.tmp bs=1048576 count=16 2>/dev/null
	$(MKMBR) --lba $@.tmp 2048:28672:0c >/dev/null
	mformat -i $@.tmp@@1048576 -F -T 28672 -v LOAD ::
	mv $@.tmp $@

# `tests/run conload`: every line typed at the console under load arrives,
# numbered and checksummed, with the reader parked, as type-ahead, after an
# interrupt character to a foreground child, and across an exec. It is the
# case of tests/cases/machine.conload.json, which etude runs on a copy of the
# load suite's disk, and has no target here.

# `tests/run fatwrite`: FAT32 written from a program, read back after a
# reboot, judged by the host's walk, mtools and fsck.fat where the host has
# it, taken through a crash and its repair, written from BusyBox, and written
# by two processes under the lag. They are the machine cases of
# tests/cases/machine.fatwrite.json, which etude runs, and have no target
# here. Each case boots a private copy of this disk: the test disk with an
# empty FAT32 volume of 14 MiB in its second partition, at sector 100352.
$(IMAGES_DIR)/fatwrite-volume.disk: krffs/manifests/disk-test.manifest $(MANIFEST_DEPS_disk_test) \
                                    $(MKDISK) build/krffs/mkfs.krffs build/krffs/stageimage.krffs \
                                    $(MKMBR) | preflight krffs tests
	@set -e; temp=$@.new.$$$$; trap 'rm -f "$$temp"' EXIT; \
	    $(MKDISK) krffs/manifests/disk-test.manifest "$$temp" 64 48; \
	    $(MKMBR) --lba "$$temp" 2048:98304:7f 100352:28672:0c; \
	    mformat -i "$$temp@@51380224" -F -T 28672 -v M37FAT ::; mv "$$temp" $@

# `tests/run loopdev`: a file system in a file, through a loop device: a FAT32,
# an ext2 and a KRFFS image on a FAT32 volume, each mounted, written and read
# back, and everything an attachment refuses. They are the cases of
# tests/cases/machine.loopdev.json, which etude runs, and have no target here.
# The artifacts of the loop device cases beyond the kernel and the test
# image, which loopdev_disks.sh makes: the FAT32, ext2 and KRFFS images in
# build/tests/system/loopdev, where the native cases take them from
# (tests/native/maps), and the disk of the FAT32 volume that holds them, the
# disk of the ext2 volume that holds the KRFFS image, and the stick. They
# hold data alone, so one set serves both architectures.
LOOPDEV_ARTIFACTS = $(addprefix build/tests/system/loopdev/,fat.img ext2.img krffs.img) \
                    $(addprefix $(CASE_DIR)/loopdev/,disk.img ext2back.img stick.img)
$(call together,$(LOOPDEV_ARTIFACTS)): tests/system/loopdev_disks.sh \
    krffs/manifests/loopimg-test.manifest $(MKMBR) $(IMAGE_TOOLS)
	sh tests/system/loopdev_disks.sh build/tests/system/loopdev $(CASE_DIR)/loopdev

# `tests/run krffsmount`: a KRFFS volume mounted by mount(2) from a program,
# read, written, read back after a reboot, left by a machine killed while
# writing and repaired by the next mount, written by two processes at once,
# refused when it is malformed, taken read-only by a write the device fails,
# mounted three at a time, and left by a stick pulled from under it. Its
# cases are in tests/cases/machine.krffsmount.json, which etude runs, and it
# has no target here.
#
# The volumes, which the native cases of tests/native/probes.cases read too,
# and the disks and the stick that the cases boot copies of; none depends on
# the architecture.
KRFFSVOL_DIR = build/tests/system/krffsvol
KRFFSVOLS = $(addprefix $(KRFFSVOL_DIR)/,volume.krffs broken.krffs badmagic.krffs badchain.krffs)
$(call together,$(KRFFSVOLS)): tests/system/krffsvol.sh krffs/manifests/krffsvol-test.manifest \
    krffs/mkimage.sh build/krffs/mkfs.krffs build/krffs/stageimage.krffs | preflight krffs
	sh tests/system/krffsvol.sh volumes
KRFFSMOUNT_DISKS = $(addprefix $(CASE_DIR)/krffsmount/,volume.disk broken.disk badmagic.disk \
                   badchain.disk region.disk three.disk stick.img)
$(KRFFSMOUNT_DISKS): $(KRFFSVOLS)
$(call together,$(KRFFSMOUNT_DISKS)): tests/system/krffsvol.sh $(MKMBR)
	sh tests/system/krffsvol.sh disks $(CASE_DIR)/krffsmount

# `tests/run ext2`: the ext2 driver booted: the probes as init on an ext2
# root, ext2probe's link and cost rows on a fixture volume the host's mke2fs
# wrote, and os-real on its ext2 root, written, killed, checked, damaged and
# repaired by recovery's e2fsck. They are the machine cases of
# tests/cases/machine.ext2.json, which etude runs, and have no target here.
# The cases boot private copies of the three disks below.
#
# The probes' disk: the test manifest as an ext2 volume of 16 MiB, the first
# MBR partition at sector 2048.
$(IMAGES_DIR)/ext2-probes.disk: $(DEMO_CREDS) krffs/manifests/test.manifest $(MANIFEST_DEPS_test) \
                                tools/image/mkext2.sh $(IMAGE_TOOLS) $(MKMBR) \
                                | preflight krffs rach2 tests musl-programs utils ish \
                                  tools/etude/guest check-dropbearmulti
	@mkdir -p $(IMAGES_DIR)
	sh tools/image/mkext2.sh krffs/manifests/test.manifest $@.vol 16
	{ dd if=/dev/zero bs=1048576 count=1 2>/dev/null; cat $@.vol; } > $@.tmp
	rm -f $@.vol
	$(MKMBR) --lba $@.tmp 2048:32768:83 > /dev/null
	mv $@.tmp $@
# The fixture disk of ext2probe's rows, which mkext2fixture.sh writes.
$(IMAGES_DIR)/ext2-fixture.disk: tests/system/mkext2fixture.sh $(MKMBR) | preflight
	@mkdir -p $(IMAGES_DIR)
	sh tests/system/mkext2fixture.sh $@.tmp $(MKMBR) > /dev/null
	mv $@.tmp $@
# The root disk: os-real as an ext2 volume of 96 MiB at sector 2048, and an
# empty ext2 volume of 32 MiB after it. The set-id probe, the helper it execs
# and the programs run beside it are test programs, not in the shipped image,
# so debugfs writes each into the volume at the root under its own name; it
# writes a file with the mode of the host's copy, so each mode is set after.
EXT2_ROOT_PROGRAMS = modeprobe suidhelper zombieprobe musl/lchownmusl musl/direntmusl
$(IMAGES_DIR)/ext2-root.disk: $(DEMO_CREDS) krffs/manifests/os-real.manifest $(MANIFEST_DEPS_os_real) \
                              $(filter $(addprefix %/,$(EXT2_ROOT_PROGRAMS)),$(MANIFEST_DEPS_test)) \
                              tools/image/mkext2.sh $(IMAGE_TOOLS) $(MAN_ALL) $(MKMBR) \
                              | preflight krffs rach2 $(MANIFEST_PARTS_os_real) tests musl-programs
	@mkdir -p $(IMAGES_DIR)
	sh tools/image/mkext2.sh krffs/manifests/os-real.manifest $@.p1 96
	. tools/setup/e2fsprogs_path.sh && for prog in $(EXT2_ROOT_PROGRAMS); do \
	    name=`basename $$prog` && \
	    debugfs -w -R "write $(ARCH_BUILD)/tests/$$prog $$name" $@.p1 > /dev/null 2>&1 && \
	    debugfs -w -R "sif $$name mode 0100755" $@.p1 > /dev/null 2>&1 && \
	    rm -f $@.check && debugfs -R "dump /$$name $@.check" $@.p1 > /dev/null 2>&1 && \
	    cmp -s $(ARCH_BUILD)/tests/$$prog $@.check || \
	    { echo "ext2-root: $$name: not written into the volume" >&2; exit 1; }; \
	done
	rm -f $@.check $@.p2 && : > $@.p2
	. tools/setup/e2fsprogs_path.sh && E2FSPROGS_FAKE_TIME=1 mke2fs -q -F -t ext2 -b 4096 -I 128 \
	    -O sparse_super,large_file,filetype,^dir_index,resize_inode,^ext_attr,^has_journal,^extent,^64bit,^flex_bg,^metadata_csum,^huge_file,^dir_nlink,^extra_isize \
	    $@.p2 32M > /dev/null 2>&1
	{ dd if=/dev/zero bs=1048576 count=1 2>/dev/null; cat $@.p1 $@.p2; \
	  dd if=/dev/zero bs=1048576 count=1 2>/dev/null; } > $@.tmp
	rm -f $@.p1 $@.p2
	$(MKMBR) --lba $@.tmp 2048:196608:83 198656:65536:83 > /dev/null
	mv $@.tmp $@

# `tests/run gnu`: the GNU system on its ext2 root, driven over OpenSSH with
# nobody at the console, logged in to by key and by password, and each port,
# GCC, GNU make, GDB, the SpiderMonkey shell and chromium-headless-shell
# doing its main job on the machine. The image is not among the products
# `prepare-all` builds: it is made from the GNU layer, on a Linux host only
# (mk/gnu-image.mk), which a set without this suite must not need, so the
# suite's own target asks for it through `os-real-gnu-ext2`. `tests/run`
# builds this target's products after `prepare-all` in a set that has the
# suite, as it does for a suite run alone, and hashes them with the rest, so
# a change to the image while the set runs voids the run.
GNU_TEST_DISK = $(IMAGES_DIR)/gnu-test.disk
run-gnu: $(DEMO_CREDS) musl-programs rach2 os-real-gnu-ext2 $(DEADLINE) $(PTYDRIVE) $(RUNUTIL) $(MKMBR)
	sh tests/system/suites/gnu.sh $(BOOT_KERNEL) $(OS_REAL_GNU_EXT2) $(GNU_TEST_DISK) \
	    $(GNU_PORT) "`cat krffs/rach2_demo_password`" "`cat krffs/rach2_test_password`"

# `tests/run codexapi`: OpenAI's Responses protocol in the fake API server,
# proved against the host's Codex at the pinned version, in a fresh home and
# a network namespace with only a loopback interface: Codex reads a file,
# runs a command, edits a file and commits, over a WebSocket and then over
# the HTTPS fallback, and the server refuses a wrong tool result. They are
# the cases of tests/cases/host.codexapi.json, which etude runs, and have no
# target here.

# `tests/run museapi`: the Responses protocol of Meta's Model API in the fake
# API server, proved against Muse Code at the pinned version, fetched and
# checked against Meta's digest, in a fresh home and a network namespace with
# only a loopback interface: Muse reads a file, runs a command, edits a file
# and commits, and the server refuses a wrong tool result. They are the cases
# of tests/cases/host.museapi.json, which etude runs, and have no target
# here.

# `tests/run agents`: the pinned GNU agent packages and their headless and
# interactive sessions, each row over SSH on a private GNU disk against its
# fake API service, as the cases of tests/cases/machine.agents.json.

# `tests/run agentsaccept`: each agent's work, cancellation, resized terminal,
# reconnected SSH client and reopened conversation, then a reboot and the
# unchanged credentials, versions and authenticated requests, as the cases
# of tests/cases/machine.agentsaccept.json.

# `tests/run fakeapi`: curl's TLS, JSON and server-sent events, a WebSocket
# exchange and an unexpected request that fails the API server's verdict,
# on a restricted GNU machine, as tests/cases/machine.fakeapi.json.

# The agent and fake API cases boot private copies of these GNU disks.
$(IMAGES_DIR)/etude-agents.disk: $(OS_REAL_GNU_AGENTS_EXT2) $(MKMBR) tests/cases/machine/agent_disk.sh
	@mkdir -p $(@D)
	sh tests/cases/machine/agent_disk.sh $< $@ $(MKMBR)

$(IMAGES_DIR)/etude-fakeapi.disk: $(OS_REAL_GNU_EXT2) $(MKMBR) tests/cases/machine/agent_disk.sh
	@mkdir -p $(@D)
	sh tests/cases/machine/agent_disk.sh $< $@ $(MKMBR)

# `tests/run exfat`: exFAT written from a program, read back after a reboot,
# judged by the host's reader and the external checker where the host has
# one, taken through a crash and its repair, written from BusyBox and by
# two processes under the lag, and unmounted after a failed write
# with its dirty bit still set. They are the machine cases of
# tests/cases/machine.exfat.json, which etude runs, and have no target here.
# Each case boots a private copy of this disk: the test disk with an empty
# exFAT volume of 16 MiB in its second partition, at sector 100352.
$(IMAGES_DIR)/exfat-volume.disk: krffs/manifests/disk-test.manifest $(MANIFEST_DEPS_disk_test) \
                                 $(MKDISK) build/krffs/mkfs.krffs build/krffs/stageimage.krffs \
                                 $(MKMBR) $(MKEXFATFIXTURE) | preflight krffs tests
	@set -e; temp=$@.new.$$$$; trap 'rm -f "$$temp" "$$temp.vol"' EXIT; \
	    $(MKDISK) krffs/manifests/disk-test.manifest "$$temp" 80 48; \
	    $(MKMBR) --lba "$$temp" 2048:98304:7f 100352:32768:07; \
	    $(MKEXFATFIXTURE) "$$temp.vol" --empty; \
	    dd if="$$temp.vol" of="$$temp" bs=512 seek=100352 conv=notrunc 2>/dev/null; \
	    mv "$$temp" $@

# `tests/run diskrate`: the storage paths timed in the machine and held under
# bounds: the raw device, a file on exFAT and on FAT32 written and read, two
# hundred small files, and a raw read piped into a hash. It is the cases of
# tests/cases/machine.diskrate.json, which etude runs, and has no target here.

# The grow set (tools/inspect/growset.c), for `tests/run grow`: the sectors
# a grow writes, checked against what the grow in QEMU changed. The tool
# includes utils/growset.c, so that file is a dependency and not a link
# input.
$(GROWSET): tools/inspect/growset.c utils/growset.c
	@mkdir -p $(dir $@)
	$(HOSTCC) $(HOSTCFLAGS) -o $@ $<
# A stick of the same shape carrying a 138 MiB disk instead of the sealed one,
# so the rows that install do not each write a gibibyte and a half. The image it
# carries has the shape the sealed disk has, a boot partition, a recovery
# partition and a 64 MiB ext2 root; a machine written from it boots, and that
# ext2 root is what the installer writes accounts into.
# The ext2 root the installer's own disk carries: the same manifest the sealed
# disk's root is built from, since its `init` is the one that starts the SSH
# server and reads the policy the installer writes. It is 64 MiB, the smallest
# that holds it, because every row that installs reads and hashes the whole
# image and the emulator hashes at a megabyte or two a second. Its 16384-block
# groups fill the first 64 MiB group, as growset requires, and it has one inode
# for each 32 KiB, 2048, twice the nodes it holds, so that the uefi suite's grow
# to 4 TiB writes 16 GiB of inode tables and not the 128 GiB mke2fs's ratio for
# a small volume would make it.
INSTALLER_EXT2 = $(IMAGES_DIR)/installer-root.ext2
$(INSTALLER_EXT2): $(FS_DEMO_IMAGES) $(DEMO_CREDS) krffs/manifests/rach2-metal-disk.manifest \
                   $(MANIFEST_DEPS_rach2_metal_disk) \
                   tools/image/mkext2.sh $(IMAGE_TOOLS) | preflight krffs rach2 utils \
                   check-busybox check-tcc check-pdpmake check-setid check-dropbearmulti \
                   check-e2fsck ports/doom
	sh tools/image/mkext2.sh krffs/manifests/rach2-metal-disk.manifest $@ 64 16384 32768
INSTALLER_DISK = $(IMAGES_DIR)/installer-test.disk
$(INSTALLER_DISK): $(DEMO_CREDS) $(KERNEL) $(RECOVERY_KERNEL) $(INSTALLER_EXT2) $(RECOVERY_IMAGE) \
                   tools/image/mkdisk_grub.sh $(DISK_TOOLS) | rach2 rach2-recovery utils krffs
	@mkdir -p $(IMAGES_DIR)
	RACH2_DISK_RECOVERY_MB=8 \
	RACH2_DISK_EXT2_MB=64 \
	RACH2_DISK_EXT2_IMAGE=$(INSTALLER_EXT2) \
	RACH2_DISK_RECOVERY_KERNEL=$(RECOVERY_KERNEL) \
	RACH2_DISK_RECOVERY_IMAGE=$(RECOVERY_IMAGE) \
	    sh tools/image/mkdisk_grub.sh $(ARCH_BUILD)/rach2/kernel $@
# A disk in the layout an earlier installer wrote: an MBR, with 64 MiB boot and
# recovery partitions at the notebook's offsets, and the boot partition at eight
# sectors to a cluster, which UEFI firmware does not read. installer-repair's
# conversion row updates it, and the grow suite takes its first 130 MiB for the
# notebook's fixture. Its name is set with the test sticks', since
# prepare-images lists it.
$(INSTALLER_MBR_DISK): $(DEMO_CREDS) $(KERNEL) $(RECOVERY_KERNEL) $(INSTALLER_EXT2) $(RECOVERY_IMAGE) \
                       tools/image/mkdisk_grub.sh $(DISK_TOOLS) | rach2 rach2-recovery utils krffs
	@mkdir -p $(IMAGES_DIR)
	RACH2_DISK_TABLE=mbr \
	RACH2_DISK_BOOT_MB=64 \
	RACH2_DISK_CLUSTER=8 \
	RACH2_DISK_RECOVERY_MB=64 \
	RACH2_DISK_EXT2_MB=64 \
	RACH2_DISK_EXT2_IMAGE=$(INSTALLER_EXT2) \
	RACH2_DISK_RECOVERY_KERNEL=$(RECOVERY_KERNEL) \
	RACH2_DISK_RECOVERY_IMAGE=$(RECOVERY_IMAGE) \
	    sh tools/image/mkdisk_grub.sh $(ARCH_BUILD)/rach2/kernel $@
# The same layout on a GUID table, the one a machine installed from the
# sealed disk has, at one sector to a cluster in the boot partition as on
# every GUID table; the grow suite takes its first 130 MiB for the machine's
# fixture. Its name is set with the test sticks', since prepare-images lists
# it.
$(GROW_GPT_DISK): $(DEMO_CREDS) $(KERNEL) $(RECOVERY_KERNEL) $(INSTALLER_EXT2) $(RECOVERY_IMAGE) \
                  tools/image/mkdisk_grub.sh $(DISK_TOOLS) | rach2 rach2-recovery utils krffs
	@mkdir -p $(IMAGES_DIR)
	RACH2_DISK_BOOT_MB=64 \
	RACH2_DISK_RECOVERY_MB=64 \
	RACH2_DISK_EXT2_MB=64 \
	RACH2_DISK_EXT2_IMAGE=$(INSTALLER_EXT2) \
	RACH2_DISK_RECOVERY_KERNEL=$(RECOVERY_KERNEL) \
	RACH2_DISK_RECOVERY_IMAGE=$(RECOVERY_IMAGE) \
	    sh tools/image/mkdisk_grub.sh $(ARCH_BUILD)/rach2/kernel $@
$(INSTALL_TEST_STICK): $(RECOVERY_ISO) $(INSTALLER_DISK) $(INSTALLER_EXT2) \
                       tools/image/mkinstallstick.sh $(MKEXFATFIXTURE) $(MKMBR) $(STICKCHECK) \
                       $(SHA256TOOL)
	sh tools/image/mkinstallstick.sh $(RECOVERY_ISO) $(INSTALLER_DISK) $@ \
	    $(MKEXFATFIXTURE) $(MKMBR) $(STICKCHECK) $(INSTALLER_EXT2)
# The images of etude's repair and growth cases
# (tests/cases/machine.installer.json and the manifests of installer-repair,
# installer-rehearse and grow), under names that do not change with the
# recovery system's key, since a case's preparation names its artifacts by a
# fixed node. Each is a link to the image.
CASE_IMAGES = $(IMAGES_DIR)/cases
CASE_IMAGE_LINKS = $(CASE_IMAGES)/install-stick.img $(CASE_IMAGES)/sealed.disk \
                   $(CASE_IMAGES)/recovery.iso $(CASE_IMAGES)/recovery.krffs
$(CASE_IMAGES)/install-stick.img: $(INSTALL_STICK)
$(CASE_IMAGES)/sealed.disk: $(SEALED_DISK)
$(CASE_IMAGES)/recovery.iso: $(RECOVERY_ISO)
$(CASE_IMAGES)/recovery.krffs: $(RECOVERY_IMAGE)
$(CASE_IMAGE_LINKS):
	@mkdir -p $(@D)
	ln -f $< $@
# The sealed disk's own name, which the install stick's manifest gives it.
$(CASE_IMAGES)/sealed.name: $(SEALED_DISK)
	@mkdir -p $(@D)
	printf '%s\n' $(notdir $(SEALED_DISK)) > $@
# The fixture of the rehearsal of the install sitting
# (machine.installer-rehearse.sitting), which rehearse_fixture.sh writes from
# the disk the test stick installs: a target disk of 256 MiB, 524288 sectors,
# marked at the first sector of each range of the save set, the save disk, the
# ranges, the root's head that growset reads and the root's offset. base.raw is
# written last, so the other files are there once it is.
REHEARSE_DIR = $(CASE_IMAGES)/rehearse
REHEARSE_FILES = $(REHEARSE_DIR)/head.vol $(REHEARSE_DIR)/params $(REHEARSE_DIR)/ranges \
                 $(REHEARSE_DIR)/save.img
$(REHEARSE_DIR)/base.raw: $(INSTALLER_DISK) tests/cases/machine/rehearse_fixture.sh $(GROWSET) \
                          $(MKEXFATFIXTURE) $(MKMBR) $(MKGPT)
	sh tests/cases/machine/rehearse_fixture.sh $(INSTALLER_DISK) 524288 $(GROWSET) \
	    $(MKEXFATFIXTURE) $(MKMBR) $(MKGPT) $(REHEARSE_DIR)
$(REHEARSE_FILES): $(REHEARSE_DIR)/base.raw
	@test -f $@
# The recovery RAM image of a tree with no key, which the installer's case
# puts on an installed disk's recovery partition, so that the key the install
# placed there is the only one its recovery system can take: the recovery
# manifest with the key's line taken out, staged as the image is. The copy
# keeps the manifest's name, by which tools/inspect/setid.allow knows it.
$(CASE_IMAGES)/recovery-keyless.krffs: $(RECOVERY_IMAGE) krffs/manifests/rach2-recovery.manifest
	@mkdir -p $(@D)/keyless
	sed '/^optional-file .* \/root\/\.ssh\/authorized_keys /d' \
	    krffs/manifests/rach2-recovery.manifest > $(@D)/keyless/rach2-recovery.manifest
	! grep -q '^optional-file.*authorized_keys' $(@D)/keyless/rach2-recovery.manifest
	$(MKIMAGE) $(@D)/keyless/rach2-recovery.manifest $@ 4
# GRUB's EFI image, which a disk the update converts must carry, copied into
# the build for tests/cases/machine.installer-repair.json, since a case's
# preparation names its artifacts by a node of the build tree.
CASE_FIXTURES = build/tests/cases/fixtures
$(CASE_FIXTURES)/bootx64.efi: vendor/grub/bootx64.efi
	@mkdir -p $(@D)
	cp $< $@
# The disks of the grow cases (tests/cases/machine.grow.json), which
# tests/system/grow_fixture.sh makes: the 16 GiB disk of a 1 GiB root the
# measured grow grows, the same disk with its entry already at the end, the
# installed machine's disk on a 64 MiB root on each table, the exFAT store of
# its grow's backups, and the recovery stick with room for them. The guest's
# wrapper of the measured grow and its checker are copied under
# build/tests/cases/grow.
$(CASE_IMAGES)/grow-base.disk: tests/system/grow_fixture.sh $(MKMBR)
	@mkdir -p $(@D)
	sh tests/system/grow_fixture.sh base $(MKMBR) $@
$(CASE_IMAGES)/grow-full.disk: $(CASE_IMAGES)/grow-base.disk tests/system/grow_fixture.sh $(MKMBR)
	sh tests/system/grow_fixture.sh full $(MKMBR) $< $@
$(CASE_IMAGES)/grow-machine-mbr.disk: $(INSTALLER_MBR_DISK) tests/system/grow_fixture.sh $(MKMBR) $(MKGPT)
	@mkdir -p $(@D)
	sh tests/system/grow_fixture.sh target mbr $< $(MKMBR) $(MKGPT) $@
$(CASE_IMAGES)/grow-machine-gpt.disk: $(GROW_GPT_DISK) tests/system/grow_fixture.sh $(MKMBR) $(MKGPT)
	@mkdir -p $(@D)
	sh tests/system/grow_fixture.sh target gpt $< $(MKMBR) $(MKGPT) $@
$(CASE_IMAGES)/grow-machine-%.store: tests/system/grow_fixture.sh tools/dev/machine_grow_plan.sh \
                                     tools/dev/machine_grow.sh $(RECOVERY_IMAGE) $(GROWSET) \
                                     $(MKEXFATFIXTURE) $(MKMBR)
	@mkdir -p $(@D)
	RACH2_ARCH=$(ARCH) sh tests/system/grow_fixture.sh store $* $(MKEXFATFIXTURE) $(MKMBR) $@
$(CASE_IMAGES)/grow-stick.img: $(RECOVERY_ISO) $(INSTALLER_DISK) tools/image/mkinstallstick.sh \
                               $(MKEXFATFIXTURE) $(MKMBR) $(STICKCHECK) $(SHA256TOOL)
	@mkdir -p $(@D)
	sh tools/image/mkinstallstick.sh $(RECOVERY_ISO) $(INSTALLER_DISK) $@ \
	    $(MKEXFATFIXTURE) $(MKMBR) $(STICKCHECK) '' 32
build/tests/cases/grow/grow_tool.sh: tests/system/fixtures/grow_tool.sh
build/tests/cases/grow/grow_measure.awk: tests/system/checkers/grow_measure.awk
build/tests/cases/grow/grow_tool.sh build/tests/cases/grow/grow_measure.awk:
	@mkdir -p $(@D)
	cp $< $@.tmp && mv $@.tmp $@
# `tests/run installer`: the install stick and the installer on the machines
# it boots, as the cases of tests/cases/machine.installer.json.
# `tests/run installer-repair`: the machine that will not boot, restored,
# updated or converted from the stick, each update also stopped part way and
# run again, as the cases of tests/cases/machine.installer-repair.json.
# `tests/run installer-rehearse`: the rehearsal of the Lenovo's install
# sitting in its small form, as the case of
# tests/cases/machine.installer-rehearse.json; tests/system/rehearse_sitting.sh
# runs it by hand at the machine's size. Etude runs the three, and they have
# no target here.
# `tests/run uefi`: the machine booted by UEFI firmware, OVMF, from its
# installed disk, from that disk's recovery entry, from the same disk with its
# boot partition's menu deleted, from the install stick's image as a USB
# device and from the recovery ISO as a CD, each answering SSH, installed from
# the stick onto a 4 TiB NVMe disk whose root grows past 2 TiB, and booted by
# SeaBIOS after recovery writes a damaged primary GUID table again.
# It is the cases of tests/cases/machine.uefi.json, which etude runs. Their
# disk is a GPT disk as the disk builder makes one for a machine, with the
# installer's root, the metal system whose init starts the SSH server.
$(ARCH_CASE_DIR)/uefi/disk.img: $(KERNEL) $(RECOVERY_KERNEL) $(RECOVERY_IMAGE) $(INSTALLER_EXT2) \
                                tools/image/mkdisk_grub.sh $(DISK_TOOLS) $(GRUB_FILES) | rach2
	@mkdir -p $(@D)
	RACH2_DISK_RECOVERY_MB=8 \
	RACH2_DISK_EXT2_MB=64 \
	RACH2_DISK_EXT2_IMAGE=$(INSTALLER_EXT2) \
	RACH2_DISK_RECOVERY_KERNEL=$(RECOVERY_KERNEL) \
	RACH2_DISK_RECOVERY_IMAGE=$(RECOVERY_IMAGE) \
	    sh tools/image/mkdisk_grub.sh $(KERNEL) $@
UEFI_DIR = $(IMAGES_DIR)/uefi
UEFI_TEST_DISK = $(IMAGES_DIR)/uefi-test.disk
UEFI_PORT := $(call port,2331)
run-uefi: $(DEMO_CREDS) rach2 utils krffs rach2-recovery $(RECOVERY_IMAGE) $(INSTALLER_EXT2) \
          $(INSTALL_TEST_STICK) $(RECOVERY_ISO) $(RUNUTIL) $(SCREENTEXT) $(DISK_TOOLS) $(MKGPT)
	@mkdir -p $(IMAGES_DIR)
	RACH2_DISK_RECOVERY_MB=8 \
	RACH2_DISK_EXT2_MB=64 \
	RACH2_DISK_EXT2_IMAGE=$(INSTALLER_EXT2) \
	RACH2_DISK_RECOVERY_KERNEL=$(RECOVERY_KERNEL) \
	RACH2_DISK_RECOVERY_IMAGE=$(RECOVERY_IMAGE) \
	    sh tools/image/mkdisk_grub.sh $(ARCH_BUILD)/rach2/kernel $(UEFI_TEST_DISK)
	sh tests/system/suites/uefi.sh $(UEFI_TEST_DISK) $(INSTALL_TEST_STICK) $(RECOVERY_ISO) \
	    $(UEFI_DIR) $(UEFI_PORT) krffs/rach2_demo_ed25519
# `tests/run usbdisk`: a flash drive as a disk, the cases of
# tests/cases/machine.usbdisk.json: sticks with an MBR, a GUID table and no
# table; the nodes, the mounts, a write, the pull from under a mount and the
# replug, ten plugs and pulls of a stick nobody opened, BusyBox mounting by
# label, a throughput line, a boot from the stick image, three sticks at once,
# raw writes from the recovery kernel, a stick past two terabytes and two
# logical units.

# The artifacts of the USB cases beyond the kernels, the images and the
# credentials: the sticks and the far volume that usbdisk_sticks.sh makes,
# the checker of the keystroke records, and the recovery image under one
# name, whether or not it has the development key.
USB_CASE_STICKS = $(addprefix $(CASE_DIR)/usbdisk/,stick-mbr.img stick-gpt.img stick-super.img \
                  stick-hub.img far.vol)
$(call together,$(USB_CASE_STICKS)): tests/system/usbdisk_sticks.sh $(MKEXFATFIXTURE) \
    $(MKMBR) $(MKGPT)
	sh tests/system/usbdisk_sticks.sh $(CASE_DIR)/usbdisk $(MKEXFATFIXTURE)
$(CASE_DIR)/usblatency.awk: tests/system/checkers/usblatency.awk
	@mkdir -p $(@D)
	cp $< $@
$(IMAGES_DIR)/usbdisk-recovery.krffs: $(RECOVERY_IMAGE)
	cp $< $@
# `tests/run diskpolicy`: the read-only kernel does not write the disk, shown on
# the bytes rather than on a return value. It is the cases of
# tests/cases/machine.diskpolicy.json, which etude runs, and has no target
# here: copies of one disk are booted with each kernel; the ordinary one must
# change its copy, which is what makes the other half mean anything, and the
# metal one must refuse a command by name and leave its copy untouched.
# `tests/run metal`: unattended SSH on the metal image, the acceptance script
# rehearsed on it, and the script's installed-root rows, the cases of
# tests/cases/machine.metal.json.

# The artifacts of the metal cases beyond the kernels, the metal image and
# the credentials: the three sticks of the unattended case, which
# metal_sticks.sh makes, and the five disks of the installed cases, which
# metal_installed_disks.sh makes from the kernel, BusyBox and bootctl of the
# architecture's build and the rows of metal_accept.sh.
METAL_STICKS = $(addprefix $(CASE_DIR)/metal/,stick.img rstick1.img rstick2.img)
$(call together,$(METAL_STICKS)): tests/system/metal_sticks.sh $(MKMBR) build/krffs/mkfs.krffs \
    build/krffs/stageimage.krffs
	sh tests/system/metal_sticks.sh $(CASE_DIR)/metal
METAL_INSTALLED = $(addprefix $(ARCH_CASE_DIR)/metal-installed/, \
                  candidate.disk gpt.disk wrong-partition.disk readonly.disk fallback.disk)
$(call together,$(METAL_INSTALLED)): tests/system/metal_installed_disks.sh \
    tests/system/accept/metal_accept.sh tools/image/mkdisk_grub.sh $(KERNEL) $(DISK_TOOLS) \
    $(GRUB_FILES) | rach2 utils check-busybox
	sh tests/system/metal_installed_disks.sh $(ARCH_BUILD) $(ARCH_CASE_DIR)/metal-installed
# `tests/run account`: accounts created, authenticated, changed and deleted on
# both images, since both have account tools and they are different code.
# os-real gets busybox's over three boots, so what is asserted is what reached
# the platter and not what one process had in its cache; os-simple gets this
# project's own in RAM, where there is no `deluser` to finish with. Both
# authenticate from an unprivileged account, since root's `su` asks for no
# password and would pass over a wrong hash and an unusable set-id `passwd`
# alike.
# Its cases are tests/cases/machine.account.json, with no target here.
ACCOUNT_TEST_DISK = $(IMAGES_DIR)/account-test.disk
# `tests/run doom`: the Doom port on a disk root, refused without its WAD,
# drawing with a zone sized from the machine's free memory, playing a demo
# to its end and recording one through fopen. Its case
# (tests/cases/machine.doom.json) boots a copy of this disk.
DOOM_TEST_DISK = $(IMAGES_DIR)/doom-test.disk
$(DOOM_TEST_DISK): krffs/manifests/doom-test.manifest $(MANIFEST_DEPS_doom_test) $(MKDISK) \
                   build/krffs/mkfs.krffs build/krffs/stageimage.krffs \
                   | preflight krffs utils busybox ports/doom
	$(MKDISK) krffs/manifests/doom-test.manifest $@ 64 48
# `tests/run tcc`: the machine compiling a program on itself. Boots os-real from
# a disk as the unprivileged `test` account, writes a C file at the prompt,
# compiles it, runs the result, and then asks for the four kinds of program this
# system cannot run and requires each to be refused with its reason and to leave
# no output behind. The boot carries `rach2.trace=enosys`, so a syscall the
# compiler wants and this kernel lacks is named on the console rather than
# turning into a mysterious failure. It is the case of
# tests/cases/machine.tcc.json, which etude runs, and has no target here.
# `tests/run make`: the machine building a program on itself, and the claim a
# compiler alone cannot make: that a second build does nothing, that touching
# one source rebuilds one object, and that the answer survives a reboot. It is
# the case of tests/cases/machine.make.json, which etude runs, and has no
# target here; the case boots one disk twice, since the reboot is half the
# point.
# The disks of the tcc and make cases, each a disk of os-real, which its case
# boots as a private copy.
$(IMAGES_DIR)/tcc-test.disk $(IMAGES_DIR)/make-test.disk: $(DEMO_CREDS) krffs/manifests/os-real.manifest \
                             $(MANIFEST_DEPS_os_real) $(IMAGE_TOOLS) $(MAN_ALL) \
                             | preflight krffs rach2 $(MANIFEST_PARTS_os_real)
	$(MKDISK) krffs/manifests/os-real.manifest $@ 64 48
# `tests/run clock`: the guest's monotonic and real clocks keeping the host's
# time within 250 ms, including reference uncertainty, across thirty stops of
# the machine's QEMU, once under the site's accelerator (see
# tests/cases/machine.clock.json). It is the case of that manifest, which
# etude runs, and has no target here.
# `tests/run smp`: the kernel on one processor, on four and on two of four, each
# finding its processors and passing a stress of fork, exec, pipes, futexes and
# munmap across them and sleeps on the last, the debug shell's preemption test
# and its ring-3 payload on two, an NMI on two that must stop the machine as a
# fault of the machine, and a parallel build on the installed system that runs
# on every one of four. They are the machine cases of
# tests/cases/machine.smp.json, which etude runs, and have no target here. The
# build case boots a private copy of this disk of os-real.
$(IMAGES_DIR)/smp-test.disk: $(DEMO_CREDS) krffs/manifests/os-real.manifest $(MANIFEST_DEPS_os_real) \
                             $(IMAGE_TOOLS) $(MAN_ALL) | preflight krffs rach2 $(MANIFEST_PARTS_os_real)
	$(MKDISK) krffs/manifests/os-real.manifest $@ 64 48
# `tests/run selfbuild`: the machine builds its own kernel, utilities and
# shell with the staged TinyCC and pdpmake, and what it built runs. It is the
# cases of tests/cases/machine.selfbuild.json, which etude runs, and has no
# target here: the test account builds the kernel twice from clean copies of
# /src/rach2 on a KRFFS root, to the same bytes, which the host takes off the
# disk and checks, and which GRUB starts to a login prompt and to the debug
# shell, where every probe of the default suite runs; the test account builds
# the utilities and the shell and runs them; and root builds the staged
# /src/ish in place.
# `tests/run selfbuild-ext2`: the machine builds its own kernel on an ext2
# root, and the kernel it built runs. It is the cases of
# tests/cases/machine.selfbuild-ext2.json: one build of the kernel on the
# os-real system as an ext2 volume, which e2fsck must find consistent after the
# synced stop, the same checks and boots of what it built, and the ten probes
# of an installed root on an ext2 root of the test manifest; and root's build
# of /src/ish in place.
#
# SELFBUILD_DISK is a disk of os-real, of which each KRFFS case boots a
# private copy, and SELFBUILD_EXT2_DISK the same system as an ext2 volume of
# 48 MiB on a partition at LBA 2048, as the KRFFS disk has it. The kernel the
# machine builds is no artifact, so it boots from a disk that GRUB starts,
# whose kernel the scenario writes. SELFBUILD_GRUB has a placeholder that
# cannot boot in the slots of its candidate and recovery entries: the
# candidate entry boots os-simple's ext2 root to a login prompt, and the
# recovery entry the test image from RAM, at whose debug shell the scenario
# runs the probes of SELFBUILD_PROBES, the PROBES table of
# tests/system/runner/runtests.c less the probes this architecture does not
# build. SELFBUILD_ROOTS has the same placeholder, and its candidate entry
# boots an ext2 root of the test manifest to the debug shell.
SELFBUILD_DISK = $(IMAGES_DIR)/selfbuild-test.disk
SELFBUILD_EXT2_VOL = $(IMAGES_DIR)/selfbuild-ext2.vol
SELFBUILD_EXT2_DISK = $(IMAGES_DIR)/selfbuild-ext2-test.disk
SELFBUILD_GRUB = $(ARCH_CASE_DIR)/selfbuild/grub.disk
SELFBUILD_ROOTS = $(ARCH_CASE_DIR)/selfbuild/roots.disk
SELFBUILD_PROBES = $(ARCH_CASE_DIR)/selfbuild/probes.list
SELFBUILD_PLACEHOLDER = no kernel: the selfbuild scenario writes the one the machine built
$(SELFBUILD_DISK): $(DEMO_CREDS) krffs/manifests/os-real.manifest $(MANIFEST_DEPS_os_real) \
                   $(IMAGE_TOOLS) $(MAN_ALL) | preflight krffs rach2 $(MANIFEST_PARTS_os_real)
	$(MKDISK) krffs/manifests/os-real.manifest $@ 64 48
$(SELFBUILD_EXT2_VOL): $(DEMO_CREDS) krffs/manifests/os-real.manifest $(MANIFEST_DEPS_os_real) \
                       tools/image/mkext2.sh $(IMAGE_TOOLS) | preflight krffs rach2 \
                       $(MANIFEST_PARTS_os_real)
	sh tools/image/mkext2.sh krffs/manifests/os-real.manifest $@ 48
$(SELFBUILD_EXT2_DISK): $(SELFBUILD_EXT2_VOL) $(MKMBR)
	{ dd if=/dev/zero bs=1048576 count=1 2>/dev/null; cat $(SELFBUILD_EXT2_VOL); \
	  dd if=/dev/zero bs=1048576 count=15 2>/dev/null; } > $@.tmp
	$(MKMBR) --lba $@.tmp 2048:98304:83 > /dev/null
	mv $@.tmp $@
$(SELFBUILD_GRUB): tools/image/mkdisk_grub.sh $(OS_SIMPLE_EXT2) $(TEST_IMAGE) $(DISK_TOOLS) \
                   $(GRUB_FILES)
	@mkdir -p $(@D)
	echo "$(SELFBUILD_PLACEHOLDER)" > $@.kernel
	RACH2_ARCH=$(ARCH) RACH2_DISK_TABLE=mbr RACH2_DISK_APPEND= RACH2_DISK_DHCP_APPEND= \
	RACH2_DISK_RECOVERY_MB=32 RACH2_DISK_EXT2_MB=16 RACH2_DISK_EXT2_IMAGE=$(OS_SIMPLE_EXT2) \
	RACH2_DISK_RECOVERY_KERNEL=$@.kernel RACH2_DISK_RECOVERY_IMAGE=$(TEST_IMAGE) \
	    sh tools/image/mkdisk_grub.sh $@.kernel $@.tmp
	rm -f $@.kernel
	mv $@.tmp $@
$(SELFBUILD_ROOTS): $(DEMO_CREDS) krffs/manifests/test.manifest $(MANIFEST_DEPS_test) \
                    tools/image/mkext2.sh tools/image/mkdisk_grub.sh $(IMAGE_TOOLS) $(DISK_TOOLS) \
                    $(GRUB_FILES) | preflight krffs rach2 tests musl-programs utils ish \
                    tools/etude/guest check-dropbearmulti
	@mkdir -p $(@D)
	sh tools/image/mkext2.sh krffs/manifests/test.manifest $@.ext2 16
	echo "$(SELFBUILD_PLACEHOLDER)" > $@.kernel
	RACH2_ARCH=$(ARCH) RACH2_DISK_TABLE=mbr RACH2_DISK_APPEND=rach2.rescue \
	RACH2_DISK_DHCP_APPEND= RACH2_DISK_RECOVERY_MB=8 RACH2_DISK_EXT2_MB=16 \
	RACH2_DISK_EXT2_IMAGE=$@.ext2 \
	    sh tools/image/mkdisk_grub.sh $@.kernel $@.tmp
	rm -f $@.ext2 $@.kernel
	mv $@.tmp $@
$(SELFBUILD_PROBES): tests/system/runner/runtests.c
	@mkdir -p $(@D)
	awk -v arch=$(ARCH) ' \
	    FNR == NR && /I686_ONLY\[\] *=/ { \
	        n = split($$0, f, "\""); for (i = 2; i < n; i += 2) only[f[i]] = 1 } \
	    FNR == NR { next } \
	    /PROBES\[\].*= *\{/ { p = 1 } \
	    p && /^[ \t]*"/ { \
	        n = split($$0, f, "\""); \
	        for (i = 2; i < n; i += 2) if (arch == "i686" || !(f[i] in only)) print f[i] } \
	    p && /};/ { p = 0 }' $< $< > $@.tmp
	mv $@.tmp $@
# `tests/run ssh`: logins over SSH with nobody at the keyboard, the case of
# tests/cases/machine.ssh.json.

# The artifact of the ssh cases beyond the kernel, the scenario, the flood and
# the demo credentials: a disk of os-real, of which each case boots a private
# copy.
$(IMAGES_DIR)/ssh-test.disk: $(DEMO_CREDS) krffs/manifests/os-real.manifest $(MANIFEST_DEPS_os_real) \
                             $(IMAGE_TOOLS) $(MAN_ALL) | preflight krffs rach2 $(MANIFEST_PARTS_os_real)
	$(MKDISK) krffs/manifests/os-real.manifest $@ 64 48

# The disk of the dhcp family's `disk` case (tests/cases/machine.dhcp.json): a
# disk of os-real, of which the case boots one private copy twice.
$(IMAGES_DIR)/dhcp-test.disk: $(DEMO_CREDS) krffs/manifests/os-real.manifest $(MANIFEST_DEPS_os_real) \
                              $(IMAGE_TOOLS) $(MAN_ALL) | preflight krffs rach2 $(MANIFEST_PARTS_os_real)
	$(MKDISK) krffs/manifests/os-real.manifest $@ 64 48

# `tests/run shutdown`: userland stopping the machine, on a disk fixture, with
# every claim of a stop asserted on QEMU's event, and every claim of a halt on
# the processors halted in QEMU's monitor, rather than on a token, every disk
# checked by offline fsck afterwards, and init killed on four processors
# leaving all four halted with interrupts off and the disk dirty. Its cases
# (tests/cases/machine.shutdown.json) boot copies of this disk.
SHUTDOWN_TEST_DISK = $(IMAGES_DIR)/shutdown-test.disk
$(SHUTDOWN_TEST_DISK): $(DEMO_CREDS) krffs/manifests/shutdown-test.manifest \
                       $(MANIFEST_DEPS_shutdown_test) $(MKDISK) build/krffs/mkfs.krffs \
                       build/krffs/stageimage.krffs | preflight krffs utils busybox
	$(MKDISK) krffs/manifests/shutdown-test.manifest $@ 64 48
# `tests/run login`: prove the console cannot fall back to an unauthenticated
# shell. Two images differing only in the init they declare, neither staging
# /bin/login, plus a rescue boot of the real image, which is the way back into
# one that will not offer a prompt.
# Its cases are tests/cases/machine.login.json, with no target here.
LOGIN_TEST_STRICT = $(IMAGES_DIR)/login-test-strict.krffs
LOGIN_TEST_SHELL  = $(IMAGES_DIR)/login-test-shell.krffs
# `tests/run man`: the manual pages on the machine. Boots os-real, logs in and
# reads every page through a pipe, which must be the staged page byte for
# byte, and rach2(7) on the console under the pager, whose first and last
# lines must appear; `man`'s exit status proves nothing, since it ignores its
# pager's. The recovery image gives the pages it stages the same way. They are
# the cases of tests/cases/machine.man.json, which etude runs, and have no
# target here.
# What the man cases count, from the rendered pages and the two tables: one
# line each of the pages, the aliases and the recovery image's pages, and
# rach2(7)'s first line and its last, which the case waits for under the
# pager.
MAN_SUMMARY = $(CASE_DIR)/man/summary
$(MAN_SUMMARY): $(MAN_ALL) man/aliases man/recovery
	@mkdir -p $(@D)
	@set -e; { \
	    printf 'pages %s\n' "`find build/man -path 'build/man/cat*' -type f | wc -l | tr -d ' '`"; \
	    printf 'aliases %s\n' "`grep -cv '^#' man/aliases`"; \
	    printf 'recovery %s\n' "`grep -cv '^#' man/recovery`"; \
	    printf 'first %s\n' "`gunzip -c build/man/cat7/rach2.7 | head -n 1`"; \
	    printf 'last %s\n' "`gunzip -c build/man/cat7/rach2.7 | tail -n 1`"; \
	} > $@.tmp && mv $@.tmp $@
# `tests/run procfiles`: the machine's own /proc files, on the boots that differ
# and through the programs that read them. Three boots of the test image, one
# per mount source, each of which must name its own; then `ps`, `df`, `free` and
# `uptime` on the real image, each of which fails when a /proc file it reads is
# missing.
# Its cases are tests/cases/machine.procfiles.json, with no target here.
# `tests/run exitline`: the plain process-exit line is printed under the debug
# shell and nowhere else. Four boots: os-real, which must stay quiet while a
# forked program runs, the test image that must still print it for a child and
# a probe, a killed probe whose dedicated marker must survive, and the harness
# kernel, whose status a reader with no token takes from the plain line. They
# are the cases of tests/cases/machine.exitline.json, which etude runs, and
# have no target here.
HARNESS_KERNEL = $(ARCH_BUILD)/rach2-harness/kernel

# The kernels a tier can be run on instead of the default one, each a component
# of its own beside the harness kernel: the UBSan-trap kernel, in which
# undefined behaviour is a `ud2` the fault handler panics on with the address,
# and the -O2 kernel, which a milestone's close boots once through the core
# tier. `tests/run` builds the one `RACH2_KERNEL` names and passes its path as
# BOOT_KERNEL, which takes the default kernel's place wherever a recipe boots
# it directly. The disks GRUB boots carry rach2/kernel, and the harness,
# recovery and metal kernels stay as they are.
.PHONY: ubsan-kernel o2-kernel
ubsan-kernel:
	$(MAKE) -C rach2 COMPONENT=rach2-ubsan KEXTRA="-fsanitize=undefined -fsanitize-undefined-trap-on-error"
o2-kernel:
	$(MAKE) -C rach2 COMPONENT=rach2-o2 KEXTRA=-O2

# The kernel built with the harness exit port, which the exitline cases and
# the usb-latency proof boot. It is a prerequisite, or a case's artifact, and
# never a step of a recipe: a recipe that ran `make` in `rach2/` while the pool
# was running would be a second build of one tree beside the first, and two
# builds of one tree write over each other. As a file rule beside the
# kernel's own it runs the sub-make once per invocation, which relinks only
# when a source changed.
$(HARNESS_KERNEL): ask-the-sub-make
	@$(MAKE) --no-print-directory -C rach2 COMPONENT=rach2-harness HARNESS=1
harness-kernel: $(HARNESS_KERNEL)
# `tests/run stress`: an on-disk KRFFS larger than the page cache. It is the
# case of tests/cases/guest.stress.json, which etude runs, and has no target
# here: stressprobe writes and reads back 33 MiB through a page cache of at
# most 8 MiB, on a private copy of etude's disk on IDE.
# The harness's arguments that put the drive drv0 on a port of an ich9-ahci
# (if=none and ide-hd), beside the fixed NIC; rach2.disk=ahci then selects the
# AHCI backend.
AHCI_ATTACH = --qemu-arg=-device --qemu-arg=ich9-ahci,id=ahci0 \
              --qemu-arg=-device --qemu-arg=ide-hd,drive=drv0,bus=ahci0.0
# `tests/run disk-ahci`: the disk suite's page-cache integrity check with the
# disk on an AHCI controller. It is the case of
# tests/cases/guest.disk-ahci.json, which etude runs, and has no target here.
# `tests/run disk-q35`: the same disk proof on the q35 machine, whose AHCI
# controller is built in at 0:31.2, function 2 of the multi-function device 31.
# That is the Lenovo's topology, and no other test exercises it: every other
# AHCI test attaches a separate controller that lands at function 0, so a scan
# that never walked past function 0 would pass all of them and fail on the
# real machine. It is the case of tests/cases/guest.disk-q35.json, which etude
# runs, and has no target here.
# `tests/run disks`: two disks of different sizes on one AHCI controller,
# both in /proc/disk with their ports and sizes, both with nodes, a
# partition of each mounted and written, and the second disk's file found
# on the second disk by the host. It is the case of
# tests/cases/machine.disks.json, which etude runs, and has no target here.
# The disks of its case, which the case's preparation names: on port 0 a
# 64 MiB disk with one KRFFS partition of the disk suites' manifest, which
# holds guest programs and so is made for each architecture, and on port 1 a
# 20 MiB disk with one empty FAT32 partition from sector 2048, which holds
# data alone.
DISKS_ZERO = $(ARCH_CASE_DIR)/disks/zero.disk
DISKS_ONE  = $(CASE_DIR)/disks/one.disk
$(DISKS_ZERO): krffs/manifests/disk-test.manifest $(MANIFEST_DEPS_disk_test) $(MKDISK) \
               build/krffs/mkfs.krffs build/krffs/stageimage.krffs | preflight krffs tests
	$(MKDISK) krffs/manifests/disk-test.manifest $@ 64 48
$(DISKS_ONE): $(MKMBR)
	@mkdir -p $(@D)
	rm -f $@.tmp
	dd if=/dev/zero of=$@.tmp bs=1048576 count=20 2>/dev/null
	$(MKMBR) --lba $@.tmp 2048:36864:0c >/dev/null
	mformat -i $@.tmp@@1048576 -F -T 36864 -v DISKONE ::
	mv $@.tmp $@
# `tests/run parts`: four tables on four AHCI disks (an MBR with logical
# partitions, a GPT, a GPT read from its backup, a blank disk), the nodes as
# the tables number them, the table read again, a 5 TiB disk's numbers past
# 32 bits in /sys/block and /proc/partitions, and the root chosen by identity:
# found when the disks are swapped, refused when two disks have it. Then the
# recovery kernel's `disk write` on five GPT disks (both headers whole, the
# primary's checksum broken, the primary's sector zeroed, and two with no
# protective MBR, one of them without its primary header too) and on an MBR
# disk whose partition reaches the last sector, each of which must be refused
# and left as it was, since the sector it writes is a backup header or a
# partition's; and on an MBR disk whose last sector no partition holds, which
# must take the round-trip. They are the cases of
# tests/cases/machine.parts.json, which etude runs, each arrangement a boot of
# its own, and have no target here.
# The disks of the partition cases, tests/cases/machine.parts.json, which
# parts_disks.sh makes. The MBR disk with logical partitions around a KRFFS
# of disk-test.manifest, and the GUID disk with os-simple's ext2 root with
# what mkgpt said of it, hold the architecture's programs. The clone of that
# table, read from its backup, and the disks of the recovery shell's `disk
# write` hold data alone, so one set of them serves both architectures. The
# blank disk and the 5 TiB one are empty disks of the cases' own.
PARTS_ARCH_DISKS = $(addprefix $(ARCH_CASE_DIR)/parts/,a.disk b.disk b.gpt)
$(call together,$(PARTS_ARCH_DISKS)): tests/system/parts_disks.sh \
    krffs/manifests/disk-test.manifest $(MANIFEST_DEPS_disk_test) $(MKDISK) build/krffs/mkfs.krffs \
    build/krffs/stageimage.krffs $(MKMBR) $(MKGPT) $(OS_SIMPLE_EXT2) | preflight krffs tests
	sh tests/system/parts_disks.sh arch $(ARCH_CASE_DIR)/parts $(OS_SIMPLE_EXT2)
PARTS_DATA_DISKS = $(addprefix $(CASE_DIR)/parts/,c.disk gpt.disk gpt-nombr.disk \
                     gpt-nombr-noprimary.disk gpt-noprimary.disk mbr-full.disk mbr-scratch.disk)
$(call together,$(PARTS_DATA_DISKS)): tests/system/parts_disks.sh $(MKMBR) $(MKGPT)
	sh tests/system/parts_disks.sh shared $(CASE_DIR)/parts
# `tests/run bus`: four IDE drives on both channels, an AHCI and an IDE disk
# together with the root on each in turn, and a disk past 128 GiB. They are
# the cases of tests/cases/machine.bus.json, which etude runs, and have no
# target here.
# The disks of its cases, which the cases' preparation names. The four IDE
# drives, of 16, 20, 24 and 28 MiB, each have one empty FAT32 partition from
# sector 2048 to the end and an MBR id of their own, and hold data alone. The
# AHCI disk and the IDE disk of the root cases are KRFFS disks of the disk
# suites' manifest, for each architecture, with the MBR ids aaaa0001 and
# bbbb0002. The disk of 130 GiB is an empty disk of its case, which the
# scenario lays out, since the store cannot hash a file of that size.
BUS_FOUR    = $(addprefix $(CASE_DIR)/bus/,pm.disk ps.disk sm.disk ss.disk)
BUS_MIB_pm  = 16
BUS_MIB_ps  = 20
BUS_MIB_sm  = 24
BUS_MIB_ss  = 28
BUS_ID_pm   = 11110000
BUS_ID_ps   = 11110001
BUS_ID_sm   = 11110002
BUS_ID_ss   = 11110003
BUS_ROOTS   = $(ARCH_CASE_DIR)/bus/sata.disk $(ARCH_CASE_DIR)/bus/ide.disk
BUS_ID_sata = aaaa0001
BUS_ID_ide  = bbbb0002
$(BUS_FOUR): $(CASE_DIR)/bus/%.disk: $(MKMBR)
	@mkdir -p $(@D)
	rm -f $@.tmp
	dd if=/dev/zero of=$@.tmp bs=1048576 count=$(BUS_MIB_$*) 2>/dev/null
	$(MKMBR) --lba --id $(BUS_ID_$*) $@.tmp 2048:$$(($(BUS_MIB_$*) * 2048 - 2048)):0c >/dev/null
	mformat -i $@.tmp@@1048576 -F -T $$(($(BUS_MIB_$*) * 2048 - 2048)) -v DISK$(BUS_ID_$*) ::
	mv $@.tmp $@
$(BUS_ROOTS): $(ARCH_CASE_DIR)/bus/%.disk: krffs/manifests/disk-test.manifest \
              $(MANIFEST_DEPS_disk_test) $(MKDISK) build/krffs/mkfs.krffs \
              build/krffs/stageimage.krffs $(MKMBR) | preflight krffs tests
	rm -f $@.tmp
	$(MKDISK) krffs/manifests/disk-test.manifest $@.tmp 64 48
	$(MKMBR) --lba --id $(BUS_ID_$*) $@.tmp 2048:98304:7f >/dev/null
	mv $@.tmp $@
# `tests/run large`: a file of 256 MiB on ext2 and on exFAT, written, read
# back after a remount, sought, sized, truncated, appended and shared between
# two descriptors, a mebibyte written astride the 4 GiB line of a sparse ext2
# file, and a write past each format's limit refused by name. They are the
# machine cases of tests/cases/machine.large.json, which etude runs, and have
# no target here. The exFAT volume comes from the host's own formatter.
# `tests/run grow`: a 1 GiB ext2 root grown by `growroot` on the recovery
# system to a partition the size of the installed machine's disk, its
# writes checked against the grow set, a grow stopped in resize2fs and
# finished, one whose entry is already at the end, and the memory it needs;
# the machine's save, grow and restore on a small root, on an MBR and on a
# GUID table; and on the host, the grow script's checks and its measures'
# checker. They are the cases of tests/cases/machine.grow.json, which etude
# runs, and have no target here.
# `tests/run queue`: real AHCI reads, interrupts, ext2 batch writes and
# fallback root faults beside raw reads; controlled host models test deadlines.
# It is the cases of tests/cases/guest.queue.json, which etude runs, and has
# no target here. Their disks are two of 8 MiB with a FAT32 partition each,
# a copy of etude's disk with a FAT32 partition after its KRFFS one, which is
# the root, and an ext2 volume of 6 MiB on a disk of 8.
QUEUE_DIR = $(IMAGES_DIR)/queue
$(QUEUE_DIR)/slow.disk $(QUEUE_DIR)/fast.disk: $(MKMBR)
	@mkdir -p $(@D)
	dd if=/dev/zero of=$@.tmp bs=1048576 count=8 2>/dev/null
	$(MKMBR) --lba $@.tmp 2048:14336:0c >/dev/null
	mformat -i $@.tmp@@1048576 -F -T 14336 ::
	mv $@.tmp $@
$(QUEUE_DIR)/root.disk: $(ETUDE_DISK) $(MKMBR)
	@mkdir -p $(@D)
	cp $(ETUDE_DISK) $@.tmp
	$(MKMBR) --lba $@.tmp 2048:98304:7f 100352:28672:0c >/dev/null
	mformat -i $@.tmp@@51380224 -F -T 28672 ::
	mv $@.tmp $@
$(QUEUE_DIR)/late.disk: $(MKMBR)
	@mkdir -p $(@D)
	rm -f $@.vol
	. tools/setup/e2fsprogs_path.sh && E2FSPROGS_FAKE_TIME=1 mke2fs -q -F -t ext2 -b 4096 -I 128 \
	    -O sparse_super,large_file,filetype,^dir_index,resize_inode,^ext_attr,^has_journal,^extent,^64bit,^flex_bg,^metadata_csum,^huge_file,^dir_nlink,^extra_isize \
	    $@.vol 6M >/dev/null
	{ dd if=/dev/zero bs=1048576 count=1 2>/dev/null; cat $@.vol; \
	  dd if=/dev/zero bs=1048576 count=1 2>/dev/null; } > $@.tmp
	rm -f $@.vol
	$(MKMBR) --lba $@.tmp 2048:12288:83 >/dev/null
	mv $@.tmp $@
# `tests/run persist-ahci`: fsync durability across a power cut with the disk
# on an AHCI controller. It is the case of
# tests/cases/machine.persist-ahci.json, which etude runs, and has no target
# here.
# `tests/run stress-ahci`: the stress suite's on-disk KRFFS larger than the page
# cache, on an AHCI controller. It is the case of
# tests/cases/guest.stress-ahci.json, which etude runs, and has no target
# here.
# `tests/run style`: the source checks over the tree, booting nothing. They are
# the cases of tests/cases/host.style.json, which etude runs, and have no
# target here.

# The host suite (see tests/system/suites/host.sh): pure kernel logic compiled
# and run natively under AddressSanitizer + UndefinedBehaviorSanitizer. It waits
# on the vendored musl for the same reason krffs does: mkhash_hosttest names the
# musl crypt in its host-deps, and that source exists only once the tarball is
# extracted.
# `tests/run host`: the host tests, kernel code compiled and run natively
# under the sanitizers, etude's QEMU launcher booting the kernel, and the fuzz
# corpora replayed through their harnesses.
# The e2fsprogs stamp is a dependency because two host tests compile the port's
# own `unix_io.c` out of the extracted tree: without it they build against
# whatever that tree last held, or fail to build at all in a fresh checkout.
# The KRFFS stager is one because mkext2_overlay_test.sh makes its volumes
# with tools/image/mkext2.sh, which stages each manifest with it. The kernel
# and the test image of ARCH are because etude_qemu_hosttest boots them under
# QEMU through etude's launcher, and the KRFFS tools because
# etude_scenario_hosttest also boots the kernel on a disk root that mkdisk.sh
# makes. casecheck is because etude_legacyset_hosttest judges the cases of its
# fixture with it; its guest cases boot the kernel and the test image too, and
# run etude-guest and rngprobe, which the test image's prerequisites build.
# etude-host runs the checks.
ETUDE_SCENARIO_DISK = $(IMAGES_DIR)/host-etude-scenario.disk
run-host: check-libc $(E2FSPROGS_STAMP) $(DEADLINE) $(ETUDEHOST) $(CASECHECK) \
          build/krffs/stageimage.krffs rach2 test-image krffs
	@$(MKDISK) krffs/manifests/etude-scenario.manifest $(ETUDE_SCENARIO_DISK) 8 6 >/dev/null
	@sh tests/system/machine_measure_test.sh
	@ARCH=$(ARCH) ./tests/system/suites/host.sh

# `tests/run runner`: the harness's selftest, then the runner's own, tests/run
# driven against a registry and a makefile of its own in a temporary
# directory (tests/system/selftest.sh). It boots no machine and reads nothing
# from the product, so it needs only the harness, runtests, the two helpers,
# and the set driver and the formatter, which the fixture's runs take from
# this tree.
run-runner: $(HARNESS) $(RUNTESTS) $(PTYDRIVE) $(HOSTPEER) $(ETUDELEGACY) $(ETUDELINES)
	@$(HARNESS) --selftest
	@./tests/system/selftest.sh

# The drivers' suite (see tests/system/drivetest.sh): drive.sh and the
# harness's waits against fake machines that choose when they are ready. It
# needs the harness and nothing that boots.
# `tests/run drive`: the drivers' waits, drive.sh and the harness against fake
# machines that choose when they are ready, booting nothing.
run-drive: $(HARNESS)
	@./tests/system/drivetest.sh

# `tests/run krffscheck`: what the offline checker says about images broken on
# purpose. No machine is booted. They are the cases of
# tests/cases/host.krffscheck.json, which etude runs, and have no target
# here.

# `tests/run imagerace`: two builds of one image at the same moment, as a
# parallel run of the suites can make them, and builds stopped at each step
# of publishing one, none of which may leave an image mixed with another
# build's files or out of date. They are the cases of
# tests/cases/host.imagerace.json, which etude runs, and have no target here.

# The eight network suites again on the second card, each the family of
# etude's cases whose machines have the e1000. The cases whose subject is
# above the driver belong to the first card's family: `netif-e1000` and
# `dhcp-e1000` have only the cases that the card changes, and `raw-e1000` has
# the case of the card alone. The probe suite is not repeated at all, since
# none of its probes sends a frame.

# `tests/run ping-e1000`: the ping suite on the e1000 card. Its case is in
# tests/cases/guest.ping-e1000.json, which etude runs, and has no target here.

# `tests/run packet-e1000`: the packet suite on the e1000 card. Its case is in
# tests/cases/guest.packet-e1000.json, which etude runs, and has no target here.

# `tests/run netif-e1000`: the netif suite's probe on the e1000 card. Its cases
# are in tests/cases/machine.netif-e1000.json, which etude runs, and have no
# target here.

# `tests/run dgram-e1000`: the dgram suite on the e1000 card. Its case is in
# tests/cases/machine.dgram-e1000.json, which etude runs, and has no target
# here.

# `tests/run netconsole-e1000`: the netconsole suite on the e1000 card, the
# cases of tests/cases/machine.netconsole-e1000.json.

# `tests/run raw-e1000`: the raw suite's card half on the e1000 card. Its case
# is in tests/cases/machine.raw-e1000.json, which etude runs, and has no target
# here.

# `tests/run dhcp-e1000`: the dhcp suite's lease on the e1000 card. Its case is
# in tests/cases/machine.dhcp-e1000.json, which etude runs, and has no target
# here.

# `tests/run ssh-e1000`: the ssh suite on the e1000 card, the case of
# tests/cases/machine.ssh-e1000.json.


# The other products a `run-` target names, built with `prepare-images` before
# a set's pool starts, so that no two pooled suites build one at once; they
# are named here, after their definitions, since a prerequisite named before
# its variable is defined expands to nothing. The style check holds every
# `run-` target's file prerequisites to `prepare-all`'s closure.
prepare-products: $(MKEXFATFIXTURE) $(MKFATFIXTURE) $(OS_SIMPLE_EXT2) $(OS_REAL_SMALL_EXT2) \
                  $(GRUB_TEST_EXT2) $(FD_HOSTTEST) \
                  $(STICK_IMAGE) $(SELFBUILD_EXT2_VOL) $(IMAGE_TOOLS)

# `tests/run textbusy`: executable files exclude writers until the last image
# ends, and bind mounts retain removed KRFFS boot-root files until last close.
# They are the cases of tests/cases/guest.textbusy.json, which etude runs on
# x86-64, and have no target here.

# Artifacts of the process and account cases in tests/cases/.
$(IMAGES_DIR)/fdlock-raw.disk: $(ETUDE_DISK) $(MKFIXTURE)
	@set -e; temp=$@.new.$$$$; trap 'rm -f "$$temp"' EXIT; \
	    cp $(ETUDE_DISK) "$$temp"; $(MKFIXTURE) boot "$$temp"; mv "$$temp" $@

$(CRED_PERSIST_DISK): $(ARCH_BUILD)/tests/musl/creddisk krffs/manifests/cred-persist-test.manifest $(MKDISK) $(IMAGE_TOOLS) | preflight rach2 musl-programs krffs
	$(MKDISK) krffs/manifests/cred-persist-test.manifest $@ 64 48

$(ACCOUNT_TEST_DISK): krffs/manifests/os-real.manifest $(MANIFEST_DEPS_os_real) $(MKDISK) $(IMAGE_TOOLS) | preflight rach2 krffs $(MANIFEST_PARTS_os_real)
	$(MKDISK) krffs/manifests/os-real.manifest $@ 64 48

$(LOGIN_TEST_STRICT): $(ARCH_BUILD)/utils/sbin/init-login krffs/manifests/login-test-strict.manifest $(IMAGE_TOOLS) | preflight utils busybox krffs
	$(MKIMAGE) krffs/manifests/login-test-strict.manifest $@ 10

$(LOGIN_TEST_SHELL): $(ARCH_BUILD)/utils/sbin/init-shell krffs/manifests/login-test-shell.manifest $(IMAGE_TOOLS) | preflight utils busybox krffs
	$(MKIMAGE) krffs/manifests/login-test-shell.manifest $@ 10

$(IMAGES_DIR)/procfiles-real.krffs: $(ARCH_BUILD)/tests/musl/procfilesmusl $(OS_REAL_IMAGE) krffs/manifests/procfiles-extra.manifest | preflight musl-programs krffs
	@set -e; temp=$@.new.$$$$; trap 'rm -f "$$temp"' EXIT; \
	    cp $(OS_REAL_IMAGE) "$$temp"; \
	    build/krffs/stageimage.krffs krffs/manifests/procfiles-extra.manifest "$$temp"; \
	    mv "$$temp" $@

$(ARCH_BUILD)/tests/musl/creddisk $(ARCH_BUILD)/tests/musl/procfilesmusl: musl-programs
