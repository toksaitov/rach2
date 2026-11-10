# tests/system/suites.mk: the artifacts of the test suites.
#
# The root Makefile includes this file. That keeps one dependency graph, which
# is what lets `tests/run authority` build the recovery kernel and the metal
# image on a clean tree: those variants exist for suites alone, and a separate
# make would have to rebuild every one of them or silently use a stale one.
#
# What belongs here: the disks, fixtures, captures and ports of the suites in
# tests/registry, which are the cases of tests/cases and which etude's
# preparation builds from the nodes their manifests name, the test tools under
# build/tests/system/, and the sentence `tests/run inventory` prints for each
# suite.
#
# What does not: anything the machine itself ships. The kernel, the userland,
# the three bootable images, the guest probe programs staged into them, and the
# validators that judge a published artifact all stay in the root, because they
# are the product rather than the means of checking it.

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

.PHONY: components harness-kernel pure64-kernel

# The host ports of the machine cases. tests/run claims a block of ports for a
# run that boots a machine before it starts anything: block N is the lock file
# /tmp/rach2-ports.N, which every account can lock, the lowest free block of
# ten is taken, and the run exports RACH2_PORT_OFFSET as N * 1000 while it
# holds the lock. etude's set driver gives the machine cases ports 2400 to
# 2999 of the block (tools/etude/Readme.md, The set driver), so no port of one
# block is a port of another, and the highest, 2999 + 9000, is below the
# ephemeral ports. The installed launcher, `etude launch`, refuses to run
# tests/run in a checkout whose suites.mk does not name RACH2_PORT_OFFSET, the
# sign of a tree whose runs claim such a block.

PCAPCHECK       = build/tests/system/pcapcheck
MKFIXTURE       = build/tests/system/mkfixture
HOSTPEER        = build/tests/system/hostpeer
WAVCHECK        = build/tests/system/wavcheck
PTYDRIVE        = build/tests/system/ptydrive
# The reader of a framebuffer console's text off QEMU's screen dump, which
# the uefi cases run (tests/system/runner/screentext.c), with the kernel's
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
$(MKFIXTURE): tests/system/fixtures/mkfixture.c tools/image/host_io.h
	@mkdir -p $(dir $@)
	$(HOST_LINK)
# The input machine.audio's tone case gives QEMU's sdl backend: three notes
# of square waves, raw 16-bit stereo at 48000 Hz (mkfixture tones).
AUDIO_TONES = build/tests/system/tones.raw
$(AUDIO_TONES): $(MKFIXTURE)
	@mkdir -p $(dir $@)
	$(MKFIXTURE) tones $@.tmp && mv $@.tmp $@ || { rm -f $@.tmp; exit 1; }
# The FAT32 fixture builder and its volume check (tests/host/mkfatfixture.c):
# the suites walk a volume the kernel wrote with `--check`.
MKFATFIXTURE = build/tests/host/mkfatfixture
$(MKFATFIXTURE): tests/host/mkfatfixture.c tools/image/host_io.h
	@mkdir -p $(dir $@)
	$(HOST_LINK)
# The host end of a connection (tests/system/runner/hostpeer.c): the echo
# server the socket suite sends to.
$(HOSTPEER): tests/system/runner/hostpeer.c
	@mkdir -p $(dir $@)
	$(ETUDE_LINK)
# What a machine played, measured off the recording QEMU's wav backend writes
# (tests/system/checkers/wavcheck.c).
$(WAVCHECK): tests/system/checkers/wavcheck.c
	@mkdir -p $(dir $@)
	$(HOST_LINK)
# The pty driver (tests/system/runner/ptydrive.c), through which the SSH,
# installer, metal and gnu cases answer their password prompts.
$(PTYDRIVE): tests/system/runner/ptydrive.c
	@mkdir -p $(dir $@)
	$(HOST_LINK)
# `tests/run default`: every guest probe, each in a boot of its own on the
# test image. They are the cases of tests/cases/guest.default.json, which
# etude runs, and have no target here: guest cases that run under etude-guest,
# and machine cases, whose scenario (tests/cases/machine/default_scenario.c)
# runs a probe as the debug shell's transient init on the console.
# `tests/run etudeexec`: Rach2 builds etude by its TinyCC recipe, and the POSIX
# executor, built the same way, passes its test there. It is the case of
# tests/cases/machine.etudeexec.json, which etude runs, and has no target here:
# it boots os-real from ETUDEEXEC_DISK with ETUDEEXEC_ARCHIVE as its second
# disk, a tar archive of etude's source, the executor's test and the files they
# need, and requires a built and then a reused version, the built etude's check
# of its fixtures and of a history to pass, and every row of the executor's
# test, the subreaper's among them, to run and pass. The archive's files are
# copied with one fixed time and archived in the order of their names, so that
# the same sources make the same bytes. COPYFILE_DISABLE and the ustar format
# keep macOS's tar from adding a `._NAME` file or a header of each file's
# extended attributes.
ETUDEEXEC_DISK = $(IMAGES_DIR)/etudeexec-test.disk
ETUDEEXEC_ARCHIVE = $(CASE_DIR)/etudeexec/etude.tar
# Python imports may create caches beside source. They are not archive inputs.
ETUDEEXEC_FILES = $(shell find tools/etude tests/native -type d -name __pycache__ -prune -o \
                  -type f ! -name '*.pyc' ! -name '*.pyo' -print) utils/sha256.c utils/sha256.h \
                  tests/cases/host/host_script_inventory.c tests/cases/host/host_script_inventory.h \
                  tests/host/etude_exec_hosttest.c tests/host/compat/fixdir_host.c \
                  tests/host/compat/fixdir_host.h tests/host/compat/pathmax_host.h \
                  tests/host/compat/tree_host.h tests/host/compat/sanitizer_env_host.h \
                  tests/auxvprobe.c tests/cases/machine/etudeexec_guest.sh \
                  tests/cases/guest.fpu.json tests/cases/machine.shutdown.json
$(ETUDEEXEC_DISK): $(DEMO_CREDS) krffs/manifests/os-real.manifest $(MANIFEST_DEPS_os_real) \
                   $(IMAGE_TOOLS) $(MAN_ALL) | preflight krffs rach2 $(MANIFEST_PARTS_os_real)
	$(MKDISK) krffs/manifests/os-real.manifest $@ 64 48
$(ETUDEEXEC_ARCHIVE): $(ETUDEEXEC_FILES)
	@mkdir -p $(@D)
	rm -rf $@.d $@.list && mkdir $@.d
	COPYFILE_DISABLE=1 tar -cf - $(ETUDEEXEC_FILES) | (cd $@.d && tar -xf -)
	find $@.d -exec touch -t 200001010000 {} +
	(cd $@.d && find . -type f | LC_ALL=C sort) > $@.list
	(cd $@.d && COPYFILE_DISABLE=1 tar --format ustar -cf - -T $(CURDIR)/$@.list) > $@.tmp
	rm -rf $@.d $@.list
	mv $@.tmp $@
# The components, the builds that turn sources into programs, in one target,
# which etude's preparation builds in a make of its own before the artifacts
# of the cases, so that every component finishes before an image or a disk
# that stages it is made. An image rule also orders each staged component
# file after its builder, so a single make restages a rebuilt program.
components: harness-kernel busybox check-busybox check-dropbearmulti check-libc \
            check-pdpmake check-setid check-tcc ish krffs musl-programs \
            ports/doom rach2 rach2-metal rach2-recovery tests tools/etude/guest utils

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
# `rach2.noxsave`, where `xsaveprobe` must find OSXSAVE clear and no
# NT_X86_XSTATE, and on `qemu64`, and the x87 state alone by `fnsave` under
# `rach2.nofxsr`, which only the i686 kernel takes, since every x86-64
# processor has FXSR. They are the cases of tests/cases/guest.fpu.json, which
# etude runs, and have no target here.
# `tests/run limits`: rseq, SCHED_BATCH and SCHED_IDLE for an ordinary user,
# guard regions, get_mempolicy and process_vm_readv and process_vm_writev
# answer as Linux's, by limitsprobe's table. It is the case of
# tests/cases/guest.limits.json, which etude runs, and has no target here.
# `tests/run dmabuf`: sealed memfd ranges exported as dma-bufs, shared
# pages across fork and close, and sync-file export, merge, info and import;
# and drmfuzz, an ordinary user's random ioctls on every node of the display
# stack, in a boot with vkms, ended with no fault the kernel records. The
# guest cases are tests/cases/guest.dmabuf.json and have no make target.
# `tests/run vgpu`: QEMU's virtio-vga-gl with a virgl renderer under Rach2's
# virtio-gpu driver: the capability sets, a context for each open, a 3D
# texture uploaded and read back, a submission's sync-file out-fence passed
# on as an in-fence, blocking and non-blocking waits, and a reset that fails
# the pending fences. The machine case is tests/cases/machine.vgpu.json and
# has no make target.

# `tests/run vdso`: the vDSO, on two processors: under `rach2.tscreliable` the
# x86-64 kernel keeps its clock from the TSC and vdsomusl's clock calls make
# no system call by /proc/stat's count, and under `rach2.notsc` each makes
# one, as every clock call does on i686, which maps no vDSO. They are the
# cases of tests/cases/guest.vdso.json, which etude runs, and have no target
# here.
# `tests/run ia32`: an i386 and an x86-64 program on the x86-64 kernel exec
# each other, pass descriptors, share a futex, trace each other, run under
# each other's seccomp filters and queue signals to each other, each in its
# own ABI's layout, by mixprobe; and i386's modify_ldt, vDSO entry and DRM
# layouts, by ldtprobe and drmcompatprobe, which the ia32 profile runs on the
# x86-64 kernel. They are the cases of tests/cases/guest.ia32.json, which
# etude runs, and have no target here.
# `tests/run bigmem`: all of an x86-64 machine's eight gigabytes is counted,
# and a program's pages fill memory past what lies below 4 GiB and read back
# as written; without 1 GiB pages, 72 GiB is counted to the direct map's
# 64 GiB. They are the machine cases of tests/cases/machine.bigmem.json,
# which etude runs, and have no target here. The 72 GiB case boots only where
# the site's policy has the line `boundary_lazy bigmem-boundary-v1`, and
# elsewhere ends incomplete before its machine starts.
# `tests/run memory`: the kernel's account of physical memory at three sizes,
# memprobe at the two ends of the range and frame allocations failed at the low
# one, the string routines swept, a machine of 8 MiB refused and kernel stacks
# run out. They are the machine cases of tests/cases/machine.memory.json, which
# etude runs, and have no target here. The case of 8 MiB boots the recovery
# kernel with an image of its own: the recovery manifest less the Wi-Fi
# programs, in 5 MiB, since QEMU loads the kernel and its image only where
# both fit in the machine's memory, and they must leave the kernel less than
# the 1 MiB it needs to manage. The copy keeps the manifest's name, by which
# tools/inspect/setid.allow knows it, and the image's name does not depend on
# whether it carries a key.
$(IMAGES_DIR)/memory-test.recovery.krffs: $(RECOVERY_IMAGE) krffs/manifests/rach2-recovery.manifest
	@mkdir -p $(@D)/memory-test
	sed -e '/ build\/vendor\/hostap\//d' -e '/ build\/vendor\/iw\//d' \
	    -e '/ build\/vendor\/libnl\//d' -e '/ build\/vendor\/openssl\//d' \
	    krffs/manifests/rach2-recovery.manifest > $(@D)/memory-test/rach2-recovery.manifest
	$(MKIMAGE) $(@D)/memory-test/rach2-recovery.manifest $@ 5
# `tests/run pools`: many pipes, ptys, sockets and processes live at once,
# and the kernel heap back where it started after a thousand each of pipes,
# ptys and sockets (see tests/poolprobe.c). They are the guest cases of
# tests/cases/guest.pools.json, which etude runs, each in a boot of its own,
# and have no target here.
# `tests/run fault`: the two probes whose whole claim is that the kernel killed
# them. vmfault writes through a pointer in no mapping, and winprobe reads the
# page-cache window, which belongs to the kernel; the default suite's forkro has
# a write to a read-only page killed the same way. They are the cases of
# tests/cases/guest.fault.json, which etude runs, each in a boot of its own, and
# have no target here: each must end on signal 11 with the kernel's record of
# the kill on the console and no panic.
# `tests/run pure64`: the x86-64 kernel without the i386 ABI, built with
# IA32=0 and booted with ia32_emulation=0, refusing an i386 program's exec
# with ENOEXEC and int $0x80 with SIGSEGV while 32-bit code still runs. They
# are the cases of tests/cases/guest.pure64.json, which etude runs, each in a
# boot of its own, and have no target here.
# `tests/run console`: the two probes that read from the console, which the
# shared probe boot cannot run because they block until something is typed.
# They are the cases of tests/cases/machine.console.json, which etude runs,
# each in a boot of its own, and have no target here: stdintest reads three
# lines and prints what it read, so the case judges the text as well as the
# count, and the third read asks for five bytes of a longer line; ttyprobe
# takes a cooked line with two erases in it, a raw byte with no newline, the
# screen's answers to its queries, and Alt with x pressed on the machine's
# keyboard.
# `tests/run screen`: the console blanked and shown again, by TIOCLINUX, by
# the escape that sets the blank time and by consoleblank=. They are the cases
# of tests/cases/machine.screen.json, which etude runs, each in a boot of its
# own, and have no target here: each is judged by the kernel's records of its
# blanks and unblanks and by QMP's screen dumps, black while the console is
# blank and showing its text before and after.
# `tests/run input`: the keyboards', mice's and tablet's event devices and
# OSS's mixer. QMP's keys on the PS/2 keyboard and on a USB keyboard reach
# /dev/input/eventN as Linux's key codes with their scan codes, the mice's and
# QEMU's tablet's moves, clicks and turns reach theirs as Linux gives them,
# the tablet with Linux's numbers, bits and axes, and /dev/mixer reads and
# sets the volume of the HDA and the SB16 driver, which a stream's start
# keeps. They
# are the cases of tests/cases/machine.input.json, which etude runs, each in a
# boot of its own that tests/cases/machine/input_scenario.c drives.
# `tests/run audio`: what the machine actually played and recorded. Its cases
# (tests/cases/machine.audio.json) play the debug shell's scale on an Intel
# HDA controller and on a Sound Blaster 16, and wavcheck measures the notes in
# QEMU's recording of each; the SB16 also plays through /dev/dsp in signed
# stereo and at OSS's defaults, judged in both recorded channels; two read
# /dev/dsp on HDA, one counting the frames against /proc/audio and one finding
# the notes of $(AUDIO_TONES), which QEMU feeds the codec, in what was read;
# another reboots a copy of the shutdown disk and reads the mixer's level that
# init kept.
# `tests/run diskfull`: a file system with no room left. The probe writes until
# the write is refused, requires `ENOSPC` rather than `EIO`, removes the file
# and writes again. It is the case of tests/cases/guest.diskfull.json, which
# etude runs in a boot of its own, since it fills the test image's root, and
# has no target here.
# `tests/run socket`: the guest opening connections of its own, checked by a
# host that answers (see tests/cases/machine.socket.json). They are the cases
# of that manifest, which etude runs, and have no target here.
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
# `tests/run watchdog`: version 2 TCO expiry on q35, observed as QMP's
# watchdog run state, and gfxtool's refusal of std-vga before MMIO.
WATCHDOG_IMAGE = $(ARCH_CASE_DIR)/watchdog/watchdog.krffs
$(WATCHDOG_IMAGE): krffs/manifests/watchdog.manifest $(ARCH_BUILD)/tests/watchdogprobe \
                   $(ARCH_BUILD)/utils/sbin/gfxtool $(IMAGE_TOOLS) | krffs
	$(MKIMAGE) krffs/manifests/watchdog.manifest $@ 4

# `tests/run i915fake`: the i915 driver on its fake chip, QEMU's edu device
# answering as the HD 4400, under the GNU root: libdrm reads 8086:0a16, Mesa's
# loader picks crocus, eglinfo exits once the render ring has run crocus's
# batches, vainfo lists Haswell's decoding profiles through Intel's i965
# driver, modetest lists no eDP connector and Haswell's three planes a pipe
# with their formats, and first light fails by name under each fault of the
# chip's debugfs file; and under the test image, i915gemprobe's X tiled buffer
# written through a GTT map reads back detiled through a CPU map, its memory
# rows run a working set past the aperture, an aperture full of busy
# bindings, a userptr object and pressure past memory to ENOMEM,
# i915execprobe's batches complete, relocate and fail by each fault, and
# i915resetprobe's hung batch is reset with its context alone charged, the
# batch behind it run, and the context banned at its third hang, after
# which the driver's debugfs files and the card's error state are read and
# written. The cases
# are tests/cases/machine.i915fake.json; the first boots gnu-test.disk.

$(ARCH_BUILD)/utils/sbin/gfxtool: utils/gfxtool.c utils/programs.mk $(LIBC_STAMP)
	$(MAKE) -C utils ../$@

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
# `tests/run seabios`: the screen of a GRUB disk that SeaBIOS boots, on a
# framebuffer by default and in the VGA's text mode from the menu's text entry.
# It is the case of tests/cases/machine.seabios.json, which etude runs, and has
# no target here: two boots of the menu case's disk, the first with the login
# prompt read off a screen dump of the mode the video BIOS prefers and Doom
# scaled onto the whole framebuffer, the second, with the text entry armed for
# one boot, in 80 by 25 cells with Doom in mode 13h.
# `tests/run authority`: which kernel may write the whole disk. The build reads
# the authority off each kernel (the rach2-recovery target), and each case boots
# one kernel with the test image and a disk over AHCI: the everyday build must
# refuse a write to /dev/disk0 with -EPERM and the sector must still be zero
# afterwards, so the refusal is proved to have written nothing rather than
# merely returned an error; the recovery build must write a sector past 2 GiB,
# read it back byte for byte, and take a 200-sector write in one call, which is
# what makes a whole-disk reflash finishable: one sector per driver call would
# turn a 500 MiB write into a million cache flushes. They are the cases of
# tests/cases/guest.authority.json, which etude runs, and have no target here.
#
# The disk of the boots, which the cases' preparation names: 4 GiB of zeros
# with an MBR whose FAT-typed entry 1 publishes /dev/bootpart, made beside its
# name and renamed into place.
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
# The disk of the menu case, tests/cases/machine.menu.json, and of the
# SeaBIOS case, tests/cases/machine.seabios.json: the GRUB disk around the
# kernel, on an MBR, with os-real's root, since every line the cases type at
# the machine is a BusyBox command, and Doom and its WAD are on that root.
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
	RACH2_DISK_RECOVERY_MB=16 \
	RACH2_DISK_EXT2_MB=96 \
	RACH2_DISK_EXT2_IMAGE=$(OS_REAL_SMALL_EXT2) \
	RACH2_DISK_RECOVERY_KERNEL=$(RECOVERY_KERNEL) \
	RACH2_DISK_RECOVERY_IMAGE=$(RECOVERY_IMAGE) \
	    sh tools/image/mkdisk_grub.sh $(KERNEL) $@

# A mixed collection may request its fixed x86-64 helper from an i686 make.
# Let the architecture's own graph check and produce the disk each time.
ifneq ($(ARCH),x86_64)
build/x86_64/tests/cases/machine/install.disk: ask-the-sub-make
	+$(MAKE) --no-print-directory ARCH=x86_64 $@
endif

# `tests/run sealed`: the disk that goes into the laptop, operated the way it
# will be operated once it is in there. It is the case of
# tests/cases/machine.sealed.json, which etude runs on a copy of the disk, and
# has no target here: an untouched boot with no fallback or recovery, then the
# whole life of the machine over SSH (a kernel sent down the connection,
# installed, tried and committed; a destroyed candidate caught by the
# fallback and repaired; a missing, a damaged and an unclean root each landing
# in recovery and repaired;
# a menu staged and then torn; the boot loader replaced through /dev/grubcore;
# the root destroyed and rewritten from recovery).
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
	RACH2_DISK_RECOVERY_MB=16 \
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
# back.
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
# `tests/run crash`: the dirty-mount flag, a spare mode bit of the root node,
# untouched by dirtyprobe's clears of the set-id bits and still set after the
# unclean stop that follows. Its case (tests/cases/machine.crash.json) boots a
# copy of this disk and reads the flag with fsck.krffs. Its own image, so that
# it cannot race another suite on a shared file under `make -j`.
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
# `tests/run acpid`: BusyBox's acpid on os-real acting on the volume keys and
# the power button. On an Intel HDA controller, logged in as root, the volume
# keys QMP presses on the PS/2 keyboard move mixer's reading and the mute key
# mutes and unmutes, and QMP's system_powerdown powers the machine off through
# init. They are the cases of tests/cases/machine.acpid.json, which etude
# runs, and have no target here.
# `tests/run raw`: the raw ICMP socket, which is what `traceroute` and root's
# `ping` are, and BusyBox's `ping` as an ordinary account, over the ICMP
# datagram socket. Its cases, each in a boot of its own, are in
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
# `tests/run keyboard`: the console's keyboard through the keymap, with kbd's
# tools, on QEMU's PS/2 keyboard and on its USB keyboard. Its cases are in
# tests/cases/machine.keyboard.json, which etude runs, and have no target here.
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

# `tests/run loopdev`: a file system in a file, through a loop device: a FAT32
# and a KRFFS image on a FAT32 volume, each mounted, written and read back, and
# everything an attachment refuses. They are the cases of
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

# `tests/run ext2`: the ext2 driver booted: the probes as init on an ext2 root,
# ext2probe's link, cost, name-cache and lookup rows on a fixture volume the
# host's mke2fs wrote, and os-real on its ext2 root, written, killed, checked,
# damaged and repaired by recovery's e2fsck, with a slow read under the lag
# device beside another file's calls and four writers under memory pressure
# judged by e2fsck. They are the machine cases of
# tests/cases/machine.ext2.json, which etude runs, and have no target here. The
# cases boot private copies of the three disks below.
#
# The probes' disk: the test manifest as an ext2 volume of 16 MiB, the first
# MBR partition at sector 2048.
$(IMAGES_DIR)/ext2-probes.disk: $(DEMO_CREDS) krffs/manifests/test.manifest $(MANIFEST_DEPS_test) \
                                tools/image/mkext2.sh $(IMAGE_TOOLS) $(MKMBR) \
                                | preflight krffs rach2 tests musl-programs utils ish \
                                  tools/etude/guest check-dropbearmulti kbd sox_ng
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
EXT2_ROOT_PROGRAMS = modeprobe suidhelper zombieprobe musl/lchownmusl musl/direntmusl lockwaitprobe
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

# `tests/run gnu`: the GNU system on its ext2 root, a boot for each case of
# tests/cases/machine.gnu.json: logins by key and by password over OpenSSH, each
# port, GCC, GNU make, GDB, the SpiderMonkey shell and chromium-headless-shell
# doing its main job, on x86-64 the i386 layer's eglinfo and vulkaninfo and a
# 32-bit Windows program under GE-Proton's default mode, and tmux at the
# console. Each case boots an overlay of
# this disk: the GNU system's volume at LBA 2048 behind a table of one
# partition, copied sparse, since the volume is 24 GiB with most of it free.
# The image is made from the GNU layer, on an x86-64 Linux host or in etude's
# tooling machine (mk/gnu-image.mk), which a set without this suite must not
# need, so only the cases' preparation asks for it. No case boots the volume
# alone, so it is an intermediate file, removed once the disk holds it, whose
# absence alone does not make the disk again; with the store's object a link
# of the disk (tools/etude/Readme.md), a tree keeps one copy of the image.
GNU_TEST_DISK = $(IMAGES_DIR)/gnu-test.disk
.INTERMEDIATE: $(GNU_TEST_EXT2)
$(GNU_TEST_DISK): $(GNU_TEST_EXT2) $(MKMBR)
	@mkdir -p $(@D)
	rm -f $@.tmp
	mib=$$(($$(wc -c < $(GNU_TEST_EXT2)) / 1048576)) && \
	dd if=/dev/zero of=$@.tmp bs=1048576 count=0 seek=$$((mib + 2)) 2>/dev/null && \
	dd if=$(GNU_TEST_EXT2) of=$@.tmp bs=1048576 seek=1 conv=notrunc,sparse 2>/dev/null && \
	$(MKMBR) --lba $@.tmp 2048:$$((mib * 2048)):83 > /dev/null
	mv $@.tmp $@
	rm -f $(GNU_TEST_EXT2)

# The files of the GNU products helper that change in ordinary work: the
# source archive and the two kernel profiles of this architecture, which
# disk-policy checks. They are kept off gnu-test.disk, so that a change to a
# test script or a kernel remakes this archive of some 20 MiB and leaves the
# image of 24 GiB as it is. The helper's cases attach the archive as a raw
# disk, disk1, and unpack it into /host-products before they read it
# (tests/cases/host/Readme.md). A ustar archive ends in zero blocks, so tar
# stops there, and its size is a whole number of sectors.
GNU_TEST_PRODUCTS = $(IMAGES_DIR)/gnu-test-products.tar
$(GNU_TEST_PRODUCTS): build/tests/host-suite/scripts-assets.tar $(KERNEL) $(METAL_KERNEL)
	rm -rf $@.d $@.tmp && mkdir -p $@.d/diskpolicy/$(ARCH)
	cp build/tests/host-suite/scripts-assets.tar $@.d/scripts-assets.tar
	cp $(KERNEL) $@.d/diskpolicy/$(ARCH)/readwrite.kernel
	cp $(METAL_KERNEL) $@.d/diskpolicy/$(ARCH)/readonly.kernel
	chmod 0644 $@.d/scripts-assets.tar $@.d/diskpolicy/$(ARCH)/*.kernel
	cd $@.d && COPYFILE_DISABLE=1 tar --format=ustar -cf ../$(@F).tmp scripts-assets.tar diskpolicy
	rm -rf $@.d
	mv $@.tmp $@

# `tests/run wifi`: Wi-Fi above the radio, a station on the installed system
# joining the access points of the Linux AP guest (tests/system/wifi) through
# a wsim radio and the two relays, in each security the guest offers, and
# keeping its addresses, routes and servers right through a rekey, a lost
# and a restarted access point, a roam, a move between networks, a pulled
# cable and an rfkill block. Its cases are in tests/cases/machine.wifi.json,
# which etude runs. Each boots this disk: the installed system's ext2 root,
# whose init-metal starts wpa_supplicant, behind a table of one partition,
# copied sparse as gnu-test.disk is. The guest, $(WIFI_GUEST_DIR), is built
# for x86-64 whatever ARCH is, and booted so beside either architecture.
WIFI_TEST_DISK = $(IMAGES_DIR)/wifi-test.disk
$(WIFI_TEST_DISK): $(SEALED_EXT2) $(MKMBR)
	@mkdir -p $(@D)
	rm -f $@.tmp
	mib=$$(($$(wc -c < $(SEALED_EXT2)) / 1048576)) && \
	dd if=/dev/zero of=$@.tmp bs=1048576 count=0 seek=$$((mib + 2)) 2>/dev/null && \
	dd if=$(SEALED_EXT2) of=$@.tmp bs=1048576 seek=1 conv=notrunc,sparse 2>/dev/null && \
	$(MKMBR) --lba $@.tmp 2048:$$((mib * 2048)):83 > /dev/null
	mv $@.tmp $@

# `tests/run wifi-gnu`: the GNU root's own wpa_supplicant joining the Linux AP
# guest's networks under WPA2, SAE and PEAP as the installed system's does,
# the case of tests/cases/machine.wifi-gnu.json, on an overlay of
# gnu-test.disk, apart from `wifi` so that the suite of the installed system
# never needs the GNU layer.

# The files the gnu cases put on the machine or serve beside it: the rows'
# script, GNU sed's tarball, which the autotools row builds, and the ntpd
# row's SNTP server.
build/tests/cases/gnu/rows.sh: tests/system/gnu_rows.sh
	@mkdir -p $(@D)
	cp $< $@
build/tests/cases/gnu/sed.tar.xz: vendor/gnu/fetch.sh vendor/gnu/sources.txt
	@mkdir -p $(@D)
	cp "`sh vendor/gnu/fetch.sh --path sed`" $@

# `tests/run webgl`: the WebGL conformance suite, versions 1.0.4 and 2.0.1, in
# chunks of 250 pages in Chromium and Firefox ESR on the GNU root under the
# virtio-vga profile, each chunk judged page by page against the Debian
# reference guest's stored run, as the cases of tests/cases/machine.webgl.json.
# `tests/run video`: H.264, VP9 and AV1 clips at 720p and 1080p in Chromium and
# Firefox ESR on the GNU root, the frames shown and dropped and the decoder
# judged against the Debian reference guest's stored plays, as the cases of
# tests/cases/machine.video.json.
# `tests/run igt`: IGT's KMS, PRIME, sync file and sync object tests that
# tests/graphics/igt-76.list names, one test program a case, on vkms, std-vga
# and virtio-vga on the GNU root, each subtest judged against the Debian
# reference guest's stored result on the same profile, as the cases of
# tests/cases/machine.igt.json.
# `tests/run modetest`: every mode of std-vga and virtio-vga set by libdrm's
# modetest on the GNU root and read back with drmcapture, at the reference's
# size and checksum, as the cases of tests/cases/machine.modetest.json.
# `tests/run piglit`: parts of piglit's quick profile on llvmpipe on the GNU
# root, each test judged against the Debian reference guest's stored result,
# as the cases of tests/cases/machine.piglit.json.
# `tests/run glmark2`: glmark2's scenes validated on llvmpipe under a headless
# Weston on the GNU root as in the Debian reference guest, with the score
# recorded, as the case of tests/cases/machine.glmark2.json.
# `tests/run venus`: dEQP-VK through Venus over the testing PC's lavapipe on
# the GNU root under the venus profile, its smoke cases and vulkaninfo's
# device, every 500th case of Mesa 25.0.7's lavapipe fraction and that
# fraction whole in eight parts, each case judged against the Debian
# reference guest's stored result, and the loss of the host's render server
# under four dEQP-VK processes, as the Debian guest sees it, as the cases of
# tests/cases/machine.venus.json.
# `tests/run vkpresent`: vkcube presenting through Venus to Weston on the GNU
# root under the venus profile, the frames it is held at read back inside
# the guest and judged against the Debian reference guest's checksums, its
# rate recorded beside the Debian guest's, as the case of
# tests/cases/machine.vkpresent.json.
# `tests/run games-native`: SuperTuxKart's profile race and 0 A.D.'s visual
# replay, the fixed replays the games layer installs, in a small form of
# 640x400, a race of 10 s to its end and the match for 10 s after its loading
# screen, on llvmpipe under a headless Weston on the GNU root under the
# virtio-vga profile, with every frame's time recorded, as the cases of
# tests/cases/machine.games-native.json; their reference is Linux's run of
# the same replays on the Lenovo, not the Debian guest's.
# Etude runs them all with graphics_scenario (tests/graphics/Readme.md). Each
# machine has graphics.ext2, the runners, the IGT list, the dEQP lists, the
# guest's environment and the references, as a read-only data disk, and a
# video case clips.ext2, the clips of clips.tar, which debian.sh makes in the
# Debian guest when this clone's cache does not hold the clips that
# clips.sha256 pins; modetest's drmcapture goes over the guest channel, which
# carries some 137 bytes a frame, too slow for the 35 MB of clips. common.sh
# writes the host's half of each record, and venus-lost.sh kills the render
# server of the device loss case's QEMU.
GRAPHICS_LISTS = igt-76.list deqp-vk-venus-smoke.txt deqp-vk-lavapipe.txt.gz deqp-vk-lavapipe.flakes
GRAPHICS_FILES = $(wildcard tests/graphics/runners/*) tests/graphics/guest-env.sh \
                 $(addprefix tests/graphics/,$(GRAPHICS_LISTS)) \
                 $(shell find tests/graphics/ref/debian tests/graphics/ref/venus-wsl -type f 2>/dev/null)
build/tests/cases/graphics/graphics.tar: $(GRAPHICS_FILES)
	@mkdir -p $(@D)
	tar -cf $@.tmp -C tests/graphics runners guest-env.sh $(GRAPHICS_LISTS) ref/debian ref/venus-wsl
	mv $@.tmp $@
build/tests/cases/graphics/common.sh: tests/graphics/runners/common.sh
	@mkdir -p $(@D)
	cp $< $@
build/tests/cases/graphics/venus-lost.sh: tests/graphics/runners/venus-lost.sh
	@mkdir -p $(@D)
	cp $< $@
build/tests/cases/graphics/clips.tar: tests/graphics/runners/clips.sha256 tests/graphics/runners/video.sh
	@mkdir -p $(@D)
	sh tests/graphics/runners/debian.sh clips -t $@.tmp
	mv $@.tmp $@
build/tests/cases/graphics/%.ext2: build/tests/cases/graphics/%.tar
	@set -e; work=; \
	trap '[ -z "$$work" ] || rm -rf "$$work"; rm -f "$@.tmp"' EXIT; \
	trap 'exit 129' HUP; trap 'exit 130' INT; trap 'exit 143' TERM; \
	mkdir -p build/tmp; work=$$(mktemp -d build/tmp/graphics-disk.XXXXXX); \
	tar -xf $< -C "$$work"; \
	kib=$$(($$(wc -c < $<) / 1024 * 5 / 4 + 8192)); \
	. tools/setup/e2fsprogs_path.sh; \
	E2FSPROGS_FAKE_TIME=1 mke2fs -q -t ext2 -F -b 4096 -O none,filetype,sparse_super,large_file \
	    -d "$$work" $@.tmp $${kib}k; \
	mv $@.tmp $@

# `tests/run kvm`: Rach2's KVM run by Debian's QEMU on the x86-64 GNU root:
# the Debian guest of tests/kvm/guests booted to its token by kvm-boot.sh,
# and kvm-unit-tests and Linux's KVM selftests run by corpus.sh as
# unit-tests.list and selftests.list say, on the processor profiles max-svm
# and host-svm, as the cases of tests/cases/machine.kvm.json, which
# kvm_scenario runs (tests/kvm/Readme.md). Each machine has kvm.ext2, the
# runners, the lists and the guest, as a read-only data disk. The guest is
# made by tests/kvm/guests/mkdebian.sh from two pinned Debian packages, a
# download of some 34 MiB kept in the clone's build/cache/gnu.
KVM_ENTRY = build/x86_64/tests/kvm/svm-entry
$(KVM_ENTRY): tests/kvm/svm-entry.c rach2/sys/kvm/kvm_abi.h $(LIBC_STAMP)
	@mkdir -p $(@D) build/tmp
	TMPDIR=$(CURDIR)/build/tmp RACH2_ARCH=x86_64 vendor/musl/muslgcc -O2 -Wall -Wextra -Werror \
	    -static -Wl,-z,noexecstack $< -o $@
KVM_RACH2_DIR = build/kvm/guests/rach2
ifeq ($(ARCH),x86_64)
build/rach2/kernel: ask-the-sub-make
	$(MAKE) --no-print-directory -C rach2 ARCH=i686
build/utils/sbin/init-shell: ask-the-sub-make
	$(MAKE) --no-print-directory ARCH=i686 musl
	$(MAKE) --no-print-directory -C utils ARCH=i686 ../build/utils/sbin/init-shell
endif
build/tests/kvm/rach2-init: tests/kvm/guests/rach2-init.c tests/probe.h
	@mkdir -p $(@D)
	i686-elf-gcc -m32 -Wall -Wextra -Werror -static -nostdlib -ffreestanding \
	    -Wl,-Ttext=0x08048000 -Wl,--build-id=none $< -o $@
$(KVM_RACH2_DIR)/kernel: build/rach2/kernel
	@mkdir -p $(@D)
	cp $< $@
$(KVM_RACH2_DIR)/ram.krffs: krffs/manifests/kvm-rach2.manifest \
                            build/tests/kvm/rach2-init build/utils/sbin/init-shell $(IMAGE_TOOLS)
	RACH2_ARCH=i686 RACH2_PATH_MAP= sh krffs/mkimage.sh $< $@ 1
KVM_GUEST_DIR = build/kvm/guests/debian
$(KVM_GUEST_DIR)/initrd: tests/kvm/guests/mkdebian.sh tests/kvm/guests/debian.txt \
                         tests/kvm/guests/debian-init vendor/gnu/debian/fetch_asset.sh \
                         build/tools/wifi/mkcpio $(SHA256TOOL)
	MKCPIO=$(WIFI_MKCPIO) sh tests/kvm/guests/mkdebian.sh $(KVM_GUEST_DIR)
$(KVM_GUEST_DIR)/vmlinuz: $(KVM_GUEST_DIR)/initrd
KVM_FILES = tests/kvm/leak.sh tests/kvm/kvm-boot.sh tests/kvm/corpus.sh tests/kvm/unit-tests.list \
            tests/kvm/selftests.list
build/tests/cases/kvm/kvm.tar: $(KVM_FILES) $(KVM_GUEST_DIR)/initrd $(KVM_GUEST_DIR)/vmlinuz $(KVM_ENTRY) \
                             $(KVM_RACH2_DIR)/kernel $(KVM_RACH2_DIR)/ram.krffs
	@mkdir -p $(@D)
	tar -cf $@.tmp -C tests/kvm $(notdir $(KVM_FILES)) \
	    -C $(CURDIR)/build/kvm guests/debian/vmlinuz guests/debian/initrd \
	    guests/rach2/kernel guests/rach2/ram.krffs \
	    -C $(CURDIR)/build/x86_64/tests/kvm svm-entry
	mv $@.tmp $@
build/tests/cases/kvm/kvm.ext2: build/tests/cases/kvm/kvm.tar
	@set -e; work=; \
	trap '[ -z "$$work" ] || rm -rf "$$work"; rm -f "$@.tmp"' EXIT; \
	trap 'exit 129' HUP; trap 'exit 130' INT; trap 'exit 143' TERM; \
	mkdir -p build/tmp; work=$$(mktemp -d build/tmp/kvm-disk.XXXXXX); \
	tar -xf $< -C "$$work"; \
	kib=$$(($$(wc -c < $<) / 1024 * 5 / 4 + 8192)); \
	. tools/setup/e2fsprogs_path.sh; \
	E2FSPROGS_FAKE_TIME=1 mke2fs -q -t ext2 -F -b 4096 -O none,filetype,sparse_super,large_file \
	    -d "$$work" $@.tmp $${kib}k; \
	mv $@.tmp $@

# `tests/run bed-vtest`: the testbed's first rung (bed/Readme.md), Mesa's
# virpipe on the x86-64 GNU root drawing a triangle through vtestrelay to the
# vtest server of a Debian guest that the root's QEMU boots, under QEMU's TCG
# and under Rach2's KVM, as the cases of tests/cases/machine.bed-vtest.json,
# which bed_vtest_scenario runs. Each machine has bed-vtest.ext2 as a
# read-only data disk: the guest's cloud image and prepared image, the
# triangle's source and the runner, which bed/fixture/vtest-guest.sh stages,
# preparing the guest first when its inputs changed. The cloud image, 419 MiB,
# and the snapshot's packages are downloaded once into the main clone's
# build/cache.
BED_VTEST_FILES = bed/fixture/vtest-guest.sh bed/fixture/vtest-rach2.sh bed/fixture/triangle.c \
                  bed/fixture/setup.sh.in bed/fixture/packages bed/fixture/user-data.in \
                  bed/fixture/meta-data bed/fixture/network-config tests/graphics/debian-ref/image
build/tests/cases/bed-vtest/bed-vtest.ext2: $(BED_VTEST_FILES)
	@set -e; work=; \
	trap '[ -z "$$work" ] || rm -rf "$$work"; rm -f "$@.tmp"' EXIT; \
	trap 'exit 129' HUP; trap 'exit 130' INT; trap 'exit 143' TERM; \
	mkdir -p $(@D) build/tmp; work=$$(mktemp -d build/tmp/bed-vtest-disk.XXXXXX); \
	sh bed/fixture/vtest-guest.sh stage "$$work/disk"; \
	kib=$$(($$(ls -ln "$$work/disk" | awk '{ n += $$5 } END { print n }') / 1024 * 5 / 4 + 8192)); \
	. tools/setup/e2fsprogs_path.sh; \
	E2FSPROGS_FAKE_TIME=1 mke2fs -q -t ext2 -F -b 4096 -O none,filetype,sparse_super,large_file \
	    -d "$$work/disk" $@.tmp $${kib}k; \
	mv $@.tmp $@

# `tests/run bed-forward`: the testbed's forward direction, a Debian guest of
# QEMU on the x86-64 GNU root drawing on Rach2's GPU through QEMU's own
# virtio-gpu, virgl and then Venus with blobs, as the cases of
# tests/cases/machine.bed-forward.json, which bedforward_scenario runs
# (bed/forward/Readme.md). Each machine has forward.ext2, the runner, the
# guest's key and the scenes with the graphics runners they use, as a
# read-only data disk, and a disk whose base is guest.qcow2, the Debian
# reference guest's prepared image, which bed/forward/guest.sh writes whole.
# That script prepares the reference guest first where this clone's cache has
# none, a download of some 440 MiB and a few minutes of apt.
BED_FORWARD_DIR = build/tests/cases/bed-forward
BED_FORWARD_RUNNERS = $(addprefix tests/graphics/runners/,vkcube.sh common.sh vkhold.c frame.py)
BED_FORWARD_SCENES = bed/forward/scene.sh bed/fixture/triangle.c tests/graphics/guest-env.sh \
                     tests/graphics/ref/venus-wsl/vkcube/frames.out $(BED_FORWARD_RUNNERS)
$(BED_FORWARD_DIR)/guest.qcow2: bed/forward/guest.sh vendor/gnu/debian/venus.list \
                                $(addprefix tests/graphics/debian-ref/,debian-ref.sh image packages \
                                    setup.sh.in user-data.in meta-data network-config)
	sh bed/forward/guest.sh $@ $(BED_FORWARD_DIR)/id_ed25519
$(BED_FORWARD_DIR)/id_ed25519: $(BED_FORWARD_DIR)/guest.qcow2
$(BED_FORWARD_DIR)/forward.ext2: bed/forward/forward.sh $(BED_FORWARD_SCENES) $(BED_FORWARD_DIR)/id_ed25519
	@set -e; work=; \
	trap '[ -z "$$work" ] || rm -rf "$$work"; rm -f "$@.tmp"' EXIT; \
	trap 'exit 129' HUP; trap 'exit 130' INT; trap 'exit 143' TERM; \
	mkdir -p build/tmp; work=$$(mktemp -d build/tmp/bed-forward.XXXXXX); \
	mkdir -p "$$work/guest/runners" "$$work/disk"; \
	cp bed/forward/scene.sh bed/fixture/triangle.c tests/graphics/guest-env.sh "$$work/guest/"; \
	cp tests/graphics/ref/venus-wsl/vkcube/frames.out "$$work/guest/vkcube.ref"; \
	cp $(BED_FORWARD_RUNNERS) "$$work/guest/runners/"; \
	tar -cf "$$work/disk/guest.tar" -C "$$work/guest" .; \
	cp -p bed/forward/forward.sh $(BED_FORWARD_DIR)/id_ed25519 "$$work/disk/"; \
	. tools/setup/e2fsprogs_path.sh; \
	E2FSPROGS_FAKE_TIME=1 mke2fs -q -t ext2 -F -b 4096 -O none,filetype,sparse_super,large_file \
	    -d "$$work/disk" $@.tmp 8192k; \
	mv $@.tmp $@

# `tests/run bed-shm`: Rach2's virtio-gpu over the region it shares with
# the test bed's VM, on the x86-64 GNU root under host-svm, as the case of
# tests/cases/machine.bed-shm.json, which bed_shm_scenario runs
# (tests/kvm/Readme.md). The machine has bed-shm.ext2, the runner and the
# probe, as a read-only data disk, and vm.disk, the VM that
# `bed/vm/image/vm.sh rach2` makes over the prepared image, raw for etude's
# overlay. The prepared image is the clone's build/cache/bed-vm, which
# `vm.sh prepare` makes when it is missing or stale: a download of the
# pinned Debian cloud image and its packages, and a boot under the host's
# KVM.
# With ARCH=x86_64 the probe is the top Makefile's, which stages it.
BED_SHM_PROBE = build/x86_64/tests/vgshmprobe
ifneq ($(ARCH),x86_64)
$(BED_SHM_PROBE): ask-the-sub-make
	@$(MAKE) --no-print-directory -C tests ARCH=x86_64 ../$(BED_SHM_PROBE)
endif
build/tests/cases/bed-shm/bed-shm.ext2: tests/kvm/bed-shm.sh $(BED_SHM_PROBE)
	@set -e; work=; \
	trap '[ -z "$$work" ] || rm -rf "$$work"; rm -f "$@.tmp"' EXIT; \
	trap 'exit 129' HUP; trap 'exit 130' INT; trap 'exit 143' TERM; \
	mkdir -p $(@D) build/tmp; work=$$(mktemp -d build/tmp/bed-shm-disk.XXXXXX); \
	cp tests/kvm/bed-shm.sh $(BED_SHM_PROBE) "$$work"/; \
	. tools/setup/e2fsprogs_path.sh; \
	E2FSPROGS_FAKE_TIME=1 mke2fs -q -t ext2 -F -b 4096 -O none,filetype,sparse_super,large_file \
	    -d "$$work" $@.tmp 8192k; \
	mv $@.tmp $@
BED_VM_FILES = $(wildcard bed/vm/image/* bed/vm/module/* bed/vm/backend/*) \
               rach2/sys/virtio/vgpushm.h tests/graphics/debian-ref/image
build/tests/cases/bed-shm/vm.disk: $(BED_VM_FILES)
	@mkdir -p $(@D)
	sh bed/vm/image/vm.sh prepare -n
	sh bed/vm/image/vm.sh rach2 -o $@

# `tests/run codexapi`: OpenAI's Responses protocol in the C fake API
# service, proved against the pinned upstream Codex in a fresh home on an
# isolated GNU Rach2 machine: two turns over a WebSocket, a refused second
# WebSocket and three turns over HTTPS, with the file, commit, credentials
# and clean repository checked, and an incorrect tool result refused.

# `tests/run museapi`: Meta's Model API in the C fake API service, proved
# against the pinned upstream Muse on an isolated GNU Rach2 machine: the
# model catalog and five HTTPS turns, with the file, commit, credentials
# and clean repository checked, and an incorrect tool result refused while
# Muse still runs, before its cancellation and confirmed cleanup.

# `tests/run agents`: the pinned GNU agent packages and their headless and
# interactive sessions, each row driven by C on a private GNU disk against its
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
$(GROWSET): tools/inspect/growset.c utils/growset.c tools/image/host_io.h
	@mkdir -p $(dir $@)
ifeq ($(OS),Windows_NT)
	sh tools/etude/compile.sh $(HOSTCC) $@ $(HOSTCFLAGS) $<
else
	$(HOSTCC) $(HOSTCFLAGS) -o $@ $<
endif
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
# a small volume would make it. Its prerequisites are the sealed root's, since
# the manifest is the same.
INSTALLER_EXT2 = $(IMAGES_DIR)/installer-root.ext2
$(INSTALLER_EXT2): $(FS_DEMO_IMAGES) $(DEMO_CREDS) krffs/manifests/rach2-metal-disk.manifest \
                   $(MANIFEST_DEPS_rach2_metal_disk) $(MAN_ALL) \
                   tools/image/mkext2.sh $(IMAGE_TOOLS) | preflight krffs rach2 utils \
                   check-busybox check-tcc check-pdpmake check-setid check-dropbearmulti \
                   check-e2fsck kbd libtsm sox_ng hostap iw ports/doom term unifont
	sh tools/image/mkext2.sh krffs/manifests/rach2-metal-disk.manifest $@ 64 16384 32768
INSTALLER_DISK = $(IMAGES_DIR)/installer-test.disk
$(INSTALLER_DISK): $(DEMO_CREDS) $(KERNEL) $(RECOVERY_KERNEL) $(INSTALLER_EXT2) $(RECOVERY_IMAGE) \
                   tools/image/mkdisk_grub.sh $(DISK_TOOLS) | rach2 rach2-recovery utils krffs
	@mkdir -p $(IMAGES_DIR)
	RACH2_DISK_RECOVERY_MB=16 \
	RACH2_DISK_EXT2_MB=64 \
	RACH2_DISK_EXT2_IMAGE=$(INSTALLER_EXT2) \
	RACH2_DISK_RECOVERY_KERNEL=$(RECOVERY_KERNEL) \
	RACH2_DISK_RECOVERY_IMAGE=$(RECOVERY_IMAGE) \
	    sh tools/image/mkdisk_grub.sh $(ARCH_BUILD)/rach2/kernel $@
# A disk in the layout an earlier installer wrote: an MBR, with 64 MiB boot and
# recovery partitions at the notebook's offsets, and the boot partition at eight
# sectors to a cluster, which UEFI firmware does not read. installer-repair's
# conversion row updates it, and the grow suite takes its first 130 MiB for the
# notebook's fixture.
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
# fixture.
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
# The fixtures of the rehearsals of the install sitting, which
# rehearse_fixture.sh writes from the disk a stick installs: a target disk
# marked at the first sector of each range of the save set, the save disk, the
# ranges, the root's head that growset reads and the root's offset. The one in
# rehearse/, for machine.installer-rehearse.sitting, is from the test stick's
# disk on a target of 256 MiB, 524288 sectors; the one in rehearse-lenovo/,
# for machine.installer-rehearse.lenovo, is from the sealed disk on a sparse
# target of the Lenovo's 488397168 sectors. The target is a qcow2 image, which
# holds only the marks, and is written last, so the other files are there
# once it is. Etude writes the save disk and keeps its digest, and a change
# to etude alone does not make the fixture again.
REHEARSE_DIR = $(CASE_IMAGES)/rehearse
REHEARSE_LENOVO_DIR = $(CASE_IMAGES)/rehearse-lenovo
REHEARSE_FILES = head.vol params ranges save.img
REHEARSE_TOOLS = tests/cases/machine/rehearse_fixture.sh $(GROWSET) $(MKEXFATFIXTURE) $(MKMBR) $(MKGPT)
$(REHEARSE_DIR)/base.qcow2: $(INSTALLER_DISK) $(REHEARSE_TOOLS) | $(ETUDE)
	sh tests/cases/machine/rehearse_fixture.sh $(INSTALLER_DISK) 524288 $(GROWSET) \
	    $(MKEXFATFIXTURE) $(MKMBR) $(MKGPT) $(ETUDE) $(REHEARSE_DIR)
$(REHEARSE_LENOVO_DIR)/base.qcow2: $(SEALED_DISK) $(REHEARSE_TOOLS) | $(ETUDE)
	sh tests/cases/machine/rehearse_fixture.sh $(SEALED_DISK) 488397168 $(GROWSET) \
	    $(MKEXFATFIXTURE) $(MKMBR) $(MKGPT) $(ETUDE) $(REHEARSE_LENOVO_DIR)
$(addprefix $(REHEARSE_DIR)/,$(REHEARSE_FILES)): $(REHEARSE_DIR)/base.qcow2
$(addprefix $(REHEARSE_LENOVO_DIR)/,$(REHEARSE_FILES)): $(REHEARSE_LENOVO_DIR)/base.qcow2
$(addprefix $(REHEARSE_DIR)/,$(REHEARSE_FILES)) $(addprefix $(REHEARSE_LENOVO_DIR)/,$(REHEARSE_FILES)):
	@test -f $@
# The recovery RAM image of a tree with no key, which the installer's case
# puts on an installed disk's recovery partition, so that the key the install
# placed there is the only one its recovery system can take: the recovery
# manifest with the key's line taken out, staged as the image is, in as many
# mebibytes. The copy keeps the manifest's name, by which
# tools/inspect/setid.allow knows it.
$(CASE_IMAGES)/recovery-keyless.krffs: $(RECOVERY_IMAGE) krffs/manifests/rach2-recovery.manifest
	@mkdir -p $(@D)/keyless
	sed '/^optional-file .* \/root\/\.ssh\/authorized_keys /d' \
	    krffs/manifests/rach2-recovery.manifest > $(@D)/keyless/rach2-recovery.manifest
	! grep -q '^optional-file.*authorized_keys' $(@D)/keyless/rach2-recovery.manifest
	$(MKIMAGE) $(@D)/keyless/rach2-recovery.manifest $@ $(RECOVERY_MIB)
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
# `tests/run datapart`: the data partition a new install makes and
# `rach2-install --data` makes room for, a file on it kept across an update,
# and the trial roots booted from it, as the case of
# tests/cases/machine.datapart.json.
# `tests/run installer-rehearse`: the rehearsal of the Lenovo's install
# sitting, in its small form with the test stick and on x86-64 at the
# machine's own size with the product's stick, as the cases of
# tests/cases/machine.installer-rehearse.json. Etude runs the four, and they
# have no target here.
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
	RACH2_DISK_RECOVERY_MB=16 \
	RACH2_DISK_EXT2_MB=64 \
	RACH2_DISK_EXT2_IMAGE=$(INSTALLER_EXT2) \
	RACH2_DISK_RECOVERY_KERNEL=$(RECOVERY_KERNEL) \
	RACH2_DISK_RECOVERY_IMAGE=$(RECOVERY_IMAGE) \
	    sh tools/image/mkdisk_grub.sh $(KERNEL) $@
# `tests/run termconsole`: term as the console session, the cases of
# tests/cases/machine.termconsole.json, which etude runs, under OVMF and
# SeaBIOS. Their disk is a GPT disk as the disk builder makes one for a
# machine, with the installer's root, the metal system, which stages term,
# and rach2.console=term in its menu, which no other test disk carries.
$(ARCH_CASE_DIR)/termconsole/disk.img: $(KERNEL) $(INSTALLER_EXT2) tools/image/mkdisk_grub.sh \
                                       $(DISK_TOOLS) $(GRUB_FILES) | rach2
	@mkdir -p $(@D)
	RACH2_DISK_RECOVERY_MB=8 \
	RACH2_DISK_EXT2_MB=64 \
	RACH2_DISK_EXT2_IMAGE=$(INSTALLER_EXT2) \
	RACH2_DISK_APPEND=rach2.console=term \
	    sh tools/image/mkdisk_grub.sh $(KERNEL) $@
# `tests/run usbdisk`: a flash drive as a disk, the cases of
# tests/cases/machine.usbdisk.json: sticks with an MBR, a GUID table and no
# table; the nodes, the mounts, a write, the pull from under a mount and the
# replug, ten plugs and pulls of a stick nobody opened, BusyBox mounting by
# label, a boot from the stick image, three sticks at once, a stick past two
# terabytes and two logical units.

# The artifacts of the USB cases beyond the kernels, the images and the
# credentials: the sticks and the far volume that usbdisk_sticks.sh makes,
# and the checker of the keystroke records.
USB_CASE_STICKS = $(addprefix $(CASE_DIR)/usbdisk/,stick-mbr.img stick-gpt.img stick-super.img \
                  stick-hub.img far.vol)
$(call together,$(USB_CASE_STICKS)): tests/system/usbdisk_sticks.sh $(MKEXFATFIXTURE) \
    $(MKMBR) $(MKGPT)
	sh tests/system/usbdisk_sticks.sh $(CASE_DIR)/usbdisk $(MKEXFATFIXTURE)
$(CASE_DIR)/usblatency.awk: tests/system/checkers/usblatency.awk
	@mkdir -p $(@D)
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
# to its end and recording one through fopen, and playing a demo with its
# sound on /dev/dsp on HDA and on the SB16. Its cases
# (tests/cases/machine.doom.json) boot copies of this disk.
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
# SELFBUILD_DISK is a disk of os-real with an 80 MiB KRFFS partition at LBA
# 2048, of which each KRFFS case boots a private copy, and SELFBUILD_EXT2_DISK
# the same system as an ext2 volume of 96 MiB on a partition at the same LBA:
# the copied tree, its objects and the kernel set aside take some 25 MiB on
# x86-64, more than os-real's 48 MiB root has free. The kernel the
# machine builds is no artifact, so it boots from a disk that GRUB starts, whose
# kernel the scenario writes. SELFBUILD_GRUB has a placeholder that cannot boot
# in the slots of its candidate and recovery entries: the candidate entry boots
# os-simple's ext2 root to a login prompt, and the recovery entry the test image
# from RAM, at whose debug shell the scenario runs the probes of
# SELFBUILD_PROBES, the list of tests/default-probes less the probes this
# architecture does not build. SELFBUILD_ROOTS has the same placeholder, and its
# candidate entry boots an ext2 root of the test manifest to the debug shell.
SELFBUILD_DISK = $(IMAGES_DIR)/selfbuild-test.disk
SELFBUILD_EXT2_VOL = $(IMAGES_DIR)/selfbuild-ext2.vol
SELFBUILD_EXT2_DISK = $(IMAGES_DIR)/selfbuild-ext2-test.disk
SELFBUILD_GRUB = $(ARCH_CASE_DIR)/selfbuild/grub.disk
SELFBUILD_ROOTS = $(ARCH_CASE_DIR)/selfbuild/roots.disk
SELFBUILD_PROBES = $(ARCH_CASE_DIR)/selfbuild/probes.list
SELFBUILD_PLACEHOLDER = no kernel: the selfbuild scenario writes the one the machine built
$(SELFBUILD_DISK): $(DEMO_CREDS) krffs/manifests/os-real.manifest $(MANIFEST_DEPS_os_real) \
                   $(IMAGE_TOOLS) $(MAN_ALL) | preflight krffs rach2 $(MANIFEST_PARTS_os_real)
	$(MKDISK) krffs/manifests/os-real.manifest $@ 96 80
$(SELFBUILD_EXT2_VOL): $(DEMO_CREDS) krffs/manifests/os-real.manifest $(MANIFEST_DEPS_os_real) \
                       tools/image/mkext2.sh $(IMAGE_TOOLS) | preflight krffs rach2 \
                       $(MANIFEST_PARTS_os_real)
	sh tools/image/mkext2.sh krffs/manifests/os-real.manifest $@ 96
$(SELFBUILD_EXT2_DISK): $(SELFBUILD_EXT2_VOL) $(MKMBR)
	{ dd if=/dev/zero bs=1048576 count=1 2>/dev/null; cat $(SELFBUILD_EXT2_VOL); \
	  dd if=/dev/zero bs=1048576 count=15 2>/dev/null; } > $@.tmp
	$(MKMBR) --lba $@.tmp 2048:196608:83 > /dev/null
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
                    tools/etude/guest check-dropbearmulti kbd sox_ng
	@mkdir -p $(@D)
	sh tools/image/mkext2.sh krffs/manifests/test.manifest $@.ext2 16
	echo "$(SELFBUILD_PLACEHOLDER)" > $@.kernel
	RACH2_ARCH=$(ARCH) RACH2_DISK_TABLE=mbr RACH2_DISK_APPEND=rach2.rescue \
	RACH2_DISK_DHCP_APPEND= RACH2_DISK_RECOVERY_MB=8 RACH2_DISK_EXT2_MB=16 \
	RACH2_DISK_EXT2_IMAGE=$@.ext2 \
	    sh tools/image/mkdisk_grub.sh $@.kernel $@.tmp
	rm -f $@.ext2 $@.kernel
	mv $@.tmp $@
$(SELFBUILD_PROBES): tests/default-probes
	@mkdir -p $(@D)
	awk -v arch=$(ARCH) '/^#/ || /^$$/ { next } \
	    NF == 1 || arch == $$2 { print $$1 }' $< > $@.tmp
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
# What the man cases count, from the rendered pages and the two tables less
# their comments and blank lines, which the guest skips: one line each of the
# pages, the aliases and the recovery image's pages, and rach2(7)'s first
# line and its last, which the case waits for under the pager.
MAN_SUMMARY = $(CASE_DIR)/man/summary
$(MAN_SUMMARY): $(MAN_ALL) man/aliases man/recovery
	@mkdir -p $(@D)
	@set -e; { \
	    printf 'pages %s\n' "`find build/man -path 'build/man/cat*' -type f | wc -l | tr -d ' '`"; \
	    printf 'aliases %s\n' "`grep -cv -e '^#' -e '^$$' man/aliases`"; \
	    printf 'recovery %s\n' "`grep -cv -e '^#' -e '^$$' man/recovery`"; \
	    printf 'first %s\n' "`gunzip -c build/man/cat7/rach2.7 | head -n 1`"; \
	    printf 'last %s\n' "`gunzip -c build/man/cat7/rach2.7 | tail -n 1`"; \
	} > $@.tmp && mv $@.tmp $@
# `tests/run procfiles`: the machine's own /proc files, on the boots that differ
# and through the programs that read them. Three boots of the test image, one
# per mount source, each of which must name its own; then `ps`, `df`, `free` and
# `uptime` on the real image, each of which fails when a /proc file it reads is
# missing.
# Its cases are tests/cases/machine.procfiles.json, with no target here.
# `tests/run acpi`: ACPICA in the kernel. On QEMU's pc and q35 machines, the
# test image's boot records that ACPICA loaded the namespace with no error
# counted and took the SCI, and QMP's system_powerdown gives the power
# button's event; a boot with rach2.noacpica leaves ACPICA out and powers off
# through \_S5. They are the cases of tests/cases/machine.acpi.json, which
# etude runs, and have no target here.

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

# The kernel built with the harness exit port, which the exitline cases
# boot. It is a prerequisite, or a case's artifact, and
# never a step of a recipe: a recipe that ran `make` in `rach2/` while the pool
# was running would be a second build of one tree beside the first, and two
# builds of one tree write over each other. As a file rule beside the
# kernel's own it runs the sub-make once per invocation, which relinks only
# when a source changed.
$(HARNESS_KERNEL): ask-the-sub-make
	@$(MAKE) --no-print-directory -C rach2 COMPONENT=rach2-harness HARNESS=1
harness-kernel: $(HARNESS_KERNEL)

# The pure 64-bit kernel, the x86-64 kernel built without the i386 ABI
# (IA32=0, rach2/Makefile), which the cases of tests/cases/guest.pure64.json
# boot: a file rule, as the harness kernel's is, for the same reason, and
# not a component, since no image stages it.
PURE64_KERNEL = $(ARCH_BUILD)/rach2-pure64/kernel
$(PURE64_KERNEL): ask-the-sub-make
	@$(MAKE) --no-print-directory -C rach2 COMPONENT=rach2-pure64 IA32=0
pure64-kernel: $(PURE64_KERNEL)
# `tests/run stress`: an on-disk KRFFS larger than the page cache. It is the
# case of tests/cases/guest.stress.json, which etude runs, and has no target
# here: stressprobe writes and reads back 33 MiB through a page cache of at
# most 8 MiB, on a private copy of etude's disk on IDE.
# The harness's arguments that put the drive drv0 on a port of an ich9-ahci
# (if=none and ide-hd), beside the fixed NIC; rach2.disk=ahci then selects the
# AHCI backend.
AHCI_ATTACH = --qemu-arg=-device --qemu-arg=ich9-ahci,id=ahci0 \
              --qemu-arg=-device --qemu-arg=ide-hd,drive=drv0,bus=ahci0.0
# `tests/run disk-q35`: blockprobe's page-cache integrity check on the q35
# machine, whose AHCI controller is built in at 0:31.2, function 2 of the
# multi-function device 31. That is the Lenovo's topology, and no other test
# exercises it: every other AHCI test attaches a separate controller that lands
# at function 0, so a scan that never walked past function 0 would pass all of
# them and fail on the real machine. It is the case of
# tests/cases/guest.disk-q35.json, which etude runs, and has no target here.
# `tests/run parts`: four tables on four AHCI disks (an MBR with logical
# partitions, a GPT, a GPT read from its backup, a blank disk), the nodes as
# the tables number them, the table read again, a 5 TiB disk's numbers past
# 32 bits in /sys/block and /proc/partitions, and the root chosen by identity:
# found when the disks are swapped, refused when two disks have it. Then the
# recovery kernel's `disk write` on two GPT disks with no protective MBR, one
# of them without its primary header too, and on an MBR disk whose partition
# reaches the last sector, each of which must be refused and left as it was,
# since the sector it writes is a backup header or a partition's; and on an
# MBR disk whose last sector no partition holds, which must take the
# round-trip. They are the cases of
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
PARTS_DATA_DISKS = $(addprefix $(CASE_DIR)/parts/,c.disk gpt-nombr.disk \
                     gpt-nombr-noprimary.disk mbr-full.disk mbr-scratch.disk)
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
# `tests/run punch`: large files freed on an ext2 volume beside allocation,
# writes, syncs and memory pressure on four processors, judged after the
# machine without that kernel. It is the machine case of
# tests/cases/machine.punch.json, which etude runs, and has no target here.
# `tests/run ext2stop`: an ext2 volume stopped under writes, judged by Rach2's
# next writable mount and its check first, then the host's e2fsck -fn and the
# sentinel files, against the same volume powered off in order. It is the
# machine case of tests/cases/machine.ext2stop.json, which etude runs, and has
# no target here.
# `tests/run grow`: a 1 GiB ext2 root grown by `growroot` on the recovery
# system to a partition the size of the installed machine's disk, its
# writes checked against the grow set, a grow stopped in resize2fs and
# finished, one whose entry is already at the end, and the memory it needs;
# the machine's save, grow and restore on a small root, on an MBR and on a
# GUID table, each also cut at a completed transfer and run again, and with
# the recovery system's RAM root full; and on the host, the grow script's
# checks, its restore at the Samsung's size and its measures' checker. They
# are the cases of tests/cases/machine.grow.json, which etude runs, and have
# no target here.
# `tests/run queue`: real AHCI reads, interrupts and fallback root faults
# beside raw reads; controlled host models test deadlines. It is the cases of
# tests/cases/guest.queue.json, which etude runs, and has no target here.
# Their disks are two of 8 MiB with a FAT32 partition each, and a copy of
# etude's disk with a FAT32 partition after its KRFFS one, which is the root.
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
# `tests/run stress-ahci`: the stress suite's on-disk KRFFS larger than the page
# cache, on an AHCI controller. It is the case of
# tests/cases/guest.stress-ahci.json, which etude runs, and has no target
# here.
# `tests/run style`: the source checks over the tree, booting nothing. They are
# the cases of tests/cases/host.style.json, which etude runs, and have no
# target here.

# `tests/run host`: the host tests, kernel code compiled and run natively
# under the sanitizers, etude's QEMU launcher booting the kernel, and the fuzz
# corpora replayed through their harnesses. They are the cases of
# tests/cases/host.host.json, which etude runs, and have no target here, with
# the checkers' selftests and the scripted checks of tests/cases/host/host.list.
# The disk of etude_scenario_hosttest, the root its real boots mount, is the
# boots case's.
ETUDE_SCENARIO_DISK = $(IMAGES_DIR)/host-etude-scenario.disk
$(ETUDE_SCENARIO_DISK): krffs/manifests/etude-scenario.manifest $(MKDISK) \
                        build/krffs/mkfs.krffs build/krffs/stageimage.krffs | preflight krffs
	@mkdir -p $(@D)
	rm -f $@.tmp
	$(MKDISK) krffs/manifests/etude-scenario.manifest $@.tmp 8 6 >/dev/null
	mv $@.tmp $@
# `tests/run esctest2`: term's parser against the pinned upstream esctest2
# driver, every method run on `term -H` and its real pty with the screen
# inspected over a socket of its own, and each result held to the exact list
# of known failures. It is the case of tests/cases/host.esctest2.json, which
# etude runs; mk/esctest2.mk packages the driver.

# `tests/run krffscheck`: what the offline checker says about images broken on
# purpose. No machine is booted. They are the cases of
# tests/cases/host.krffscheck.json, which etude runs, and have no target
# here.

# `tests/run imagerace`: two builds of one image at the same moment, as a
# parallel run of the suites can make them, and builds stopped at each step
# of publishing one, none of which may leave an image mixed with another
# build's files or out of date. They are the cases of
# tests/cases/host.imagerace.json, which etude runs, and have no target here.

# One network suite again on the second card, the family of etude's cases whose
# machines have the e1000. The cases whose subject is above the driver belong to
# the first card's family, so `netconsole-e1000` has the power-off alone: the
# driver's stop drains its ring, and the boot's records reach the card as any
# frame does. The packet, ping, dgram, raw, netif, dhcp and ssh suites are not
# repeated: the sockets, the interface ioctls, the DHCP clients and the SSH
# server are above the driver, which builds no header and sums no checksum, and
# an interface taken down is a flag the driver never sees. The netif suite's
# `twocards` case runs those ioctls and pings, connects and sends through a
# packet socket on two e1000s, and the dhcp suite's `two-networks` case takes a
# lease through each of two and reaches the SSH server through both. The probe
# suite is not repeated at all, since none of its probes sends a frame.

# `tests/run netconsole-e1000`: the netconsole suite's power-off on the e1000
# card, whose stop sends the frames it was given before it silences the card,
# the case of tests/cases/machine.netconsole-e1000.json.


# `tests/run frag6`: IPv6 fragments, extension headers and the path MTU over
# ::1: an echo past one datagram comes back whole, an unknown option and a
# routing header with a segment left are refused, overlapping fragments are
# dropped with their datagram, and a Packet Too Big lowers the path's MTU.
# They are the cases of tests/cases/guest.frag6.json, which etude runs, and
# have no target here.

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

$(ACCOUNT_TEST_DISK): $(DEMO_CREDS) krffs/manifests/os-real.manifest $(MANIFEST_DEPS_os_real) $(MKDISK) $(IMAGE_TOOLS) | preflight rach2 krffs $(MANIFEST_PARTS_os_real)
	$(MKDISK) krffs/manifests/os-real.manifest $@ 64 48

$(LOGIN_TEST_STRICT): $(DEMO_CREDS) $(ARCH_BUILD)/utils/sbin/init-login krffs/manifests/login-test-strict.manifest $(IMAGE_TOOLS) | preflight utils busybox krffs
	$(MKIMAGE) krffs/manifests/login-test-strict.manifest $@ 10

$(LOGIN_TEST_SHELL): $(DEMO_CREDS) $(ARCH_BUILD)/utils/sbin/init-shell krffs/manifests/login-test-shell.manifest $(IMAGE_TOOLS) | preflight utils busybox krffs
	$(MKIMAGE) krffs/manifests/login-test-shell.manifest $@ 10

# os-real with the applet probe of machine.procfiles, in os-real's size: its
# manifest without Doom's WAD, which the case never reads, and with the probe,
# so that the probe takes none of the room os-real keeps free (Makefile).
# The copy keeps the manifest's name, by which tools/inspect/setid.allow
# knows it.
$(IMAGES_DIR)/procfiles-real.krffs: $(ARCH_BUILD)/tests/musl/procfilesmusl $(DEMO_CREDS) krffs/manifests/os-real.manifest $(MANIFEST_DEPS_os_real) krffs/manifests/procfiles-extra.manifest $(IMAGE_TOOLS) $(MAN_ALL) | preflight musl-programs krffs rach2 $(MANIFEST_PARTS_os_real)
	@mkdir -p $(@D)/procfiles
	{ sed '/^optional-file .* \/demo\/doom1\.wad /d' krffs/manifests/os-real.manifest; \
	    cat krffs/manifests/procfiles-extra.manifest; } > $(@D)/procfiles/os-real.manifest
	! grep -q 'doom1\.wad' $(@D)/procfiles/os-real.manifest
	$(MKIMAGE) $(@D)/procfiles/os-real.manifest $@ $(OS_REAL_MIB)

$(ARCH_BUILD)/tests/musl/creddisk $(ARCH_BUILD)/tests/musl/procfilesmusl: musl-programs

# `tests/run d3d`: Direct3D 9, 11 and 12 pixels on lavapipe through GE-Proton.
# `tests/run d3d-wine`: Wine's Direct3D units on both paths against Debian.
# `tests/run d3d-vkd3d`: vkd3d-proton's units on lavapipe against Debian.
# `tests/run d3dvenus`: the same probes, Wine units and vkd3d-proton units on
# Venus against Debian on Venus.
include tests/graphics/d3d/rules.mk

# `tests/run games-wine`: GE-Proton's Wine through rach2-game under Xwayland
# on virgl: a new prefix with WineD3D's video memory set, GL and Direct3D 9
# pixels, 64-bit and 32-bit, against the Debian guest's in the same QEMU, and
# the preloader's reserved ranges against Linux's maps.
# `tests/run games-wm`: in the boot session's Weston on kiosk-shell, xplace's
# and Wine's fullscreen windows placed, focused, scaled and dumped through
# QMP, raw motion under a confining grab, Wine's tone through the loopback
# and DirectInput's devices; the same payload, with wm.py.
include tests/system/games/rules.mk

# `tests/run games-umu`: the Wine probes' 64-bit Direct3D 9 program through
# rach2-game --umu, umu-launcher and the Steam Linux Runtime's container: the
# runtime unpacked once with its updates off, the direct route's Proton
# settings, Linux's pixels, the GL probe on virgl, Wine's tone through the
# container's PulseAudio socket recorded through the HDA loopback, the session
# bus, and no refused call in pressure-vessel's log. It reads the payload
# games-wine makes.

# `tests/run desktop`: an ordinary headless Weston session at boot, input
# discovery, and browser and ALSA tones measured through OSS loopback; Weston
# at boot on bochs with Pixman and on virtio-gpu with GL, the browsers taking
# keys and a click through QMP, a resolution change, and the console's frame
# and keyboard back after Weston's SIGKILL; on i686, Weston on bochs, evdev,
# an llvmpipe render checked pixel by pixel and modetest.
DESKTOP_TEST_DISK = $(IMAGES_DIR)/desktop-test.disk
$(DESKTOP_TEST_DISK): $(OS_REAL_GNU_EXT2) $(MKMBR)
	@mkdir -p $(@D)
	rm -f $@.tmp
	mib=$$(($$(wc -c < $(OS_REAL_GNU_EXT2)) / 1048576)) && \
	dd if=/dev/zero of=$@.tmp bs=1048576 count=0 seek=$$((mib + 2)) 2>/dev/null && \
	dd if=$(OS_REAL_GNU_EXT2) of=$@.tmp bs=1048576 seek=1 conv=notrunc,sparse 2>/dev/null && \
	$(MKMBR) --lba $@.tmp 2048:$$((mib * 2048)):83 > /dev/null
	mv $@.tmp $@

build/tests/graphics/desktop.py: tests/graphics/desktop.py
	@mkdir -p $(@D)
	cp $< $@

# `tests/run webgpu`: Chromium adapter answers on Venus, compared with Debian.

# `tests/run browser`: HTTPS pages, browser sandbox diagnostics, WebGPU,
# concurrent sound, GPU recovery, the fixed WebGL and video comparisons, and
# the accelerated browsers' checks on the fake i915 chip, which also read the
# Direct3D payload's disk.
BROWSER_TEST_EXT2 = $(IMAGES_DIR)/browser-test.ext2
BROWSER_TEST_DISK = $(IMAGES_DIR)/browser-test.disk
$(BROWSER_TEST_EXT2): $(GNU_IMAGE_DEPS) $(GNU_ROOT_MANIFEST_DEPS) $(GNU_AGENTS_LIST) $(GNU_AGENTS_STAMPS) | $(GNU_IMAGE_ORDER)
	RACH2_ARCH=$(ARCH) GNU_AGENTS="$(GNU_AGENTS_ON)" sh tools/image/mkext2.sh \
	    --overlay vendor/gnu/install_root.sh krffs/manifests/os-real-gnu.manifest $@ 16384
$(BROWSER_TEST_DISK): $(BROWSER_TEST_EXT2) $(MKMBR)
	@mkdir -p $(@D)
	rm -f $@.tmp
	mib=$$(($$(wc -c < $(BROWSER_TEST_EXT2)) / 1048576)) && \
	dd if=/dev/zero of=$@.tmp bs=1048576 count=0 seek=$$((mib + 2)) 2>/dev/null && \
	dd if=$(BROWSER_TEST_EXT2) of=$@.tmp bs=1048576 seek=1 conv=notrunc,sparse 2>/dev/null && \
	$(MKMBR) --lba $@.tmp 2048:$$((mib * 2048)):83 > /dev/null
	mv $@.tmp $@
BROWSER_FILES = $(wildcard tests/graphics/browser/*) $(GRAPHICS_FILES) \
                $(wildcard tests/graphics/browser/ref/*/*) $(wildcard tests/graphics/lenovo/*)
build/tests/cases/browser/seat.so: tests/graphics/browser/seat.c tests/graphics/browser/seat-build.sh
	sh tests/graphics/browser/seat-build.sh $@
build/tests/cases/browser/files.tar.gz: $(BROWSER_FILES) build/tests/cases/browser/seat.so
	@mkdir -p $(@D)
	python3 tests/graphics/browser/assets.py $@

build/tests/cases/browser/data.ext2: build/tests/cases/browser/files.tar.gz
	@set -e; work=; \
	trap '[ -z "$$work" ] || rm -rf "$$work"; rm -f "$@.tmp"' EXIT; \
	trap 'exit 129' HUP; trap 'exit 130' INT; trap 'exit 143' TERM; \
	mkdir -p build/tmp; work=$$(mktemp -d build/tmp/browser-data.XXXXXX); \
	tar -xzf $< -C "$$work"; mkdir -p "$$work/clips"; \
	mke2fs -q -t ext2 -F -b 4096 -O none,filetype,sparse_super,large_file \
	    -d "$$work" $@.tmp 65536; \
	mv $@.tmp $@

build/tests/cases/browser/clips.ext2: build/tests/cases/graphics/clips.tar
	@set -e; work=; \
	trap '[ -z "$$work" ] || rm -rf "$$work"; rm -f "$@.tmp"' EXIT; \
	trap 'exit 129' HUP; trap 'exit 130' INT; trap 'exit 143' TERM; \
	mkdir -p build/tmp; work=$$(mktemp -d build/tmp/browser-clips.XXXXXX); \
	tar -xf $< -C "$$work"; \
	mke2fs -q -t ext2 -F -b 4096 -O none,filetype,sparse_super,large_file \
	    -d "$$work" $@.tmp 65536; \
	mv $@.tmp $@

build/tests/graphics/desktop_display.py: tests/graphics/desktop_display.py
	@mkdir -p $(@D)
	cp $< $@

# `tests/run gpucompat`: i386 GPU driver calls on i915's fault backend and
# virgl, with syncobj, udmabuf, dma-buf and sync-file requests, run on the
# x86-64 kernel by RACH2_KERNEL=ia32; tests/cases/guest.gpucompat.json.

# The GNU PI row compiles the musl probe's source against glibc in the guest.
build/tests/cases/gnu/pi-futex.c: tests/musl/pifutexmusl.c
	@mkdir -p $(@D)
	cp $< $@

# `tests/run assign`: a NIC assigned to a Debian guest through VT-d and VFIO,
# DMA and MSI isolation under host traffic, release after a killed guest, and
# the Intel driver's function given to a guest and taken back.
ASSIGN_DIR = build/tests/cases/assign
$(ASSIGN_DIR)/initrd: tests/assign/mkguest.sh tests/assign/guest-init \
                      tests/assign/grub.cfg tests/assign/guest.txt \
                      tests/kvm/guests/debian.txt build/tools/wifi/mkcpio $(SHA256TOOL)
	sh tests/assign/mkguest.sh $(@D)
# The guest's own disk comes with its initramfs, from the same script.
$(ASSIGN_DIR)/grub.img: $(ASSIGN_DIR)/initrd
	@test -f $@ || sh tests/assign/mkguest.sh $(@D)
$(ASSIGN_DIR)/edu: tests/assign/edu.c rach2/sys/vfio/vfio_abi.h $(LIBC_STAMP)
	@mkdir -p $(@D) build/tmp
	TMPDIR=$(CURDIR)/build/tmp RACH2_ARCH=x86_64 vendor/musl/muslgcc -O2 -Wall -Wextra -Werror \
	    -static -Wl,-z,noexecstack $< -o $@
$(ASSIGN_DIR)/assign.tar: tests/assign/run.sh tests/assign/gpu.sh $(ASSIGN_DIR)/initrd \
                        $(ASSIGN_DIR)/grub.img $(ASSIGN_DIR)/edu \
                        $(KVM_GUEST_DIR)/vmlinuz tests/kvm/kvm-boot.sh
	tar -cf $@.tmp -C tests assign/run.sh assign/gpu.sh kvm/kvm-boot.sh \
	    -C $(CURDIR)/$(ASSIGN_DIR)/.. assign/initrd assign/grub.img assign/edu \
	    -C $(CURDIR)/build/kvm/guests/debian vmlinuz
	mv $@.tmp $@
$(ASSIGN_DIR)/assign.ext2: $(ASSIGN_DIR)/assign.tar
	@set -e; work=; \
	trap '[ -z "$$work" ] || rm -rf "$$work"; rm -f "$@.tmp"' EXIT; \
	trap 'exit 129' HUP; trap 'exit 130' INT; trap 'exit 143' TERM; \
	mkdir -p build/tmp; work=$$(mktemp -d build/tmp/assign-disk.XXXXXX); \
	tar -xf $< -C "$$work"; mv "$$work/vmlinuz" "$$work/assign/vmlinuz"; \
	kib=$$(($$(wc -c < $<) / 1024 * 5 / 4 + 8192)); \
	. tools/setup/e2fsprogs_path.sh; \
	E2FSPROGS_FAKE_TIME=1 mke2fs -q -t ext2 -F -b 4096 -O none,filetype,sparse_super,large_file \
	    -d "$$work" $@.tmp $${kib}k; \
	mv $@.tmp $@

# Assignment needs the product's GNU root, without the GNU test inventories
# or a person's staged logins. Give it a fixed MBR identity so the AHCI
# payload may be enumerated before the NVMe root.
ASSIGN_ROOT = $(ARCH_BUILD)/tests/cases/assign/root.ext2
ASSIGN_DISK = $(ARCH_BUILD)/tests/cases/assign/gnu.disk
$(ASSIGN_ROOT): $(GNU_IMAGE_DEPS) $(GNU_ROOT_MANIFEST_DEPS) $(GNU_AGENTS_LIST) $(GNU_AGENTS_STAMPS) | $(GNU_IMAGE_ORDER)
	RACH2_ARCH=$(ARCH) GNU_AGENTS="$(GNU_AGENTS_ON)" GNU_I386=$(GNU_I386_ON) \
	    sh tools/image/mkext2.sh --overlay vendor/gnu/install_root.sh \
	    krffs/manifests/os-real-gnu.manifest $@ 16384
$(ASSIGN_DISK): $(ASSIGN_ROOT) $(MKMBR)
	@mkdir -p $(@D)
	@set -e; trap 'rm -f "$@.tmp"' EXIT; \
	mib=$$(($$(wc -c < $<) / 1048576)); \
	dd if=/dev/zero of=$@.tmp bs=1048576 count=0 seek=$$((mib + 2)) 2>/dev/null; \
	dd if=$< of=$@.tmp bs=1048576 seek=1 conv=notrunc,sparse 2>/dev/null; \
	$(MKMBR) --lba --id 82274701 $@.tmp 2048:$$((mib * 2048)):83; \
	mv $@.tmp $@

# `tests/run wakelat`: measure six timed waits under idle, user, system-call
# and mixed load on four CPUs, with the timer, queue and lock counts.
