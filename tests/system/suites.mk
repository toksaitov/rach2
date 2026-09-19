# tests/system/suites.mk: how each test suite is built and run.
#
# The root Makefile includes this file. That keeps one dependency graph, which
# is what lets `tests/run authority` build the recovery kernel and the metal
# image on a clean tree: those variants exist for suites alone, and a separate
# make would have to rebuild every one of them or silently use a stale one.
#
# What belongs here: the `run-NAME` target of every suite in tests/registry,
# the disks, captures, logs and ports those suites write, the eight test tools
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

# No file is named `run-<suite>`, so every suite target is phony. The names are
# read from the registry rather than written out again: `tests/run style`
# already requires the registry and the targets in this file to name the same
# suites, so a list derived from one cannot fall behind the other.
.PHONY: prepare-all prepare-images components harness-kernel \
        $(addprefix run-,$(shell awk -F'|' '!/^#/ && NF > 1 { print $$1 }' tests/registry))

# --- RAM-disk images. Each is built out of tree from a checked-in manifest
#     by the manifest-driven stageimage.krffs (see krffs/mkimage.sh and
#     krffs/manifests/), which writes the whole tree directly as KRFFS v2
#     nodes (no FUSE mount). Each image is its own file, so they coexist. ---
# The card the network suites attach. The RTL8139 is the default, so every
# invocation that names nothing runs what it always ran; `tests/run e1000`
# re-enters these same targets with the other emulated card. The runner and
# the harness both take --device, and ssh.sh takes the card as an argument,
# because it builds its own QEMU command line.
# The host ports the suites forward. They are named here because two suites
# sharing one cannot both run, and a number buried in a recipe is a collision
# nobody sees until the two are scheduled together.
#
# The network suites run twice, once per card, and the two runs are ordinary
# neighbours rather than one waiting for the other. So the card picks the
# ports and tags the files each run writes: without that, the second-card ping
# and the first-card ping forward the same port and capture into the same
# file, and neither can say which of them wrote what.
DISKDIAG_PORT    = 2225
METAL_PORT       = 2223
SEALED_PORT      = 2297
DRILL_PORT       = 2298
PROCFILES_PORT   = 2299
ifeq ($(NIC),e1000)
NIC_TAG          = -e1000
SSH_TEST_PORT   ?= 2322
DHCP_SSH_PORT    = 2324
DHCP_TWO_PORT_A  = 2326
DHCP_TWO_PORT_B  = 2328
DGRAM_PORT       = 15898
NETIF_PORT       = 15899
else
NIC_TAG          =
SSH_TEST_PORT   ?= 2222
DHCP_SSH_PORT    = 2224
DHCP_TWO_PORT_A  = 2226
DHCP_TWO_PORT_B  = 2228
DGRAM_PORT       = 15998
NETIF_PORT       = 15999
endif
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
# The stick and disk tools (their rules are with the usbdisk suite), named
# here because a prerequisite list expands its variables where it is read.
MKGPT           = build/tools/image/mkgpt
GROWSET         = build/tools/inspect/growset
MKMBR           = build/tools/image/mkmbr
STICKCHECK      = build/tools/image/stickcheck
# The deadline tool: the suites' wall-clock limit with process-group kill,
# replacing coreutils' gtimeout. Built with the host compiler like the KRFFS
# tools.
$(DEADLINE): tests/system/runner/deadline.c
	@mkdir -p $(dir $@)
	cc -O2 -Wall -Wextra -o $@ $<
# The capture checker (tests/system/checkers/pcapcheck.c): what the guest put on
# the wire, judged from QEMU's filter-dump file.
$(PCAPCHECK): tests/system/checkers/pcapcheck.c
	@mkdir -p $(dir $@)
	cc -O2 -Wall -Wextra -o $@ $<
# The disk fixtures (tests/system/fixtures/mkfixture.c): the bootprobe partition
# pattern and the 260 GB sparse disk.
$(MKFIXTURE): tests/system/fixtures/mkfixture.c
	@mkdir -p $(dir $@)
	cc -O2 -Wall -Wextra -o $@ $<
# The FAT32 fixture builder and its volume check (tests/host/mkfatfixture.c):
# the suites walk a volume the kernel wrote with `--check`.
MKFATFIXTURE = build/tests/host/mkfatfixture
$(MKFATFIXTURE): tests/host/mkfatfixture.c
	@mkdir -p $(dir $@)
	cc -O2 -Wall -Wextra -o $@ $<
# The exFAT builder and reader (tests/host/mkexfatfixture.c), for `tests/run
# exfat`: it builds the empty volume the suite writes and walks and reads
# what the kernel wrote.
MKEXFATFIXTURE = build/tests/host/mkexfatfixture
$(MKEXFATFIXTURE): tests/host/mkexfatfixture.c tests/host/exfat_upcase.inc
	@mkdir -p $(dir $@)
	cc -O2 -Wall -Wextra -o $@ $<
# The host end of a connection (tests/system/runner/hostpeer.c): the live TCP
# connection procfiles looks for in /proc/net/tcp, and the echo server the
# socket suite sends to.
$(HOSTPEER): tests/system/runner/hostpeer.c
	@mkdir -p $(dir $@)
	cc -O2 -Wall -Wextra -o $@ $<
# What a machine played, measured off the recording QEMU's wav backend writes
# (tests/system/checkers/wavcheck.c). The maths library is the only one any
# test tool here links.
$(WAVCHECK): tests/system/checkers/wavcheck.c
	@mkdir -p $(dir $@)
	cc -O2 -Wall -Wextra -o $@ $< -lm
# The three runners (tests/system/runner/): the serial harness every suite
# drives, the probe runner one boot scores, and the pty driver the SSH suites
# answer their password prompts through.
$(HARNESS): tests/system/runner/harness.c
	@mkdir -p $(dir $@)
	cc -O2 -Wall -Wextra -o $@ $<
$(RUNTESTS): tests/system/runner/runtests.c
	@mkdir -p $(dir $@)
	cc -O2 -Wall -Wextra -o $@ $<
$(PTYDRIVE): tests/system/runner/ptydrive.c
	@mkdir -p $(dir $@)
	cc -O2 -Wall -Wextra -o $@ $<
$(RUNUTIL): tests/system/runner/runutil.c
	@mkdir -p $(dir $@)
	cc -O2 -Wall -Wextra -o $@ $<
# `make test`: the automated end-to-end pass over the test image. Boots it
# headless (-cpu max) and runs every probe, failing if any does not exit 0. The
# few probes with a special contract (fork-exit-code, stdin, faults, network)
# are run by hand under tests/system/runner/harness.c; see
# tests/system/runner/runtests.c. --timeout is one deadline for the whole run,
# not per probe, so it is stated here rather than left at the default: the suite
# grows every milestone, and a default sized for an older, shorter suite fails
# the tail as a hang on a slow host. --steptmo is the per-probe window, and it
# is stated here for one probe: musl/pwauthmusl waits out the credential lock's
# real ten-second retry budget twice, so tens of seconds is its GREEN path. Only
# a failing probe ever waits a step window out, so widening it costs a passing
# run nothing. The default suite's build-and-run target. `make test` and
# `tests/run default` both land here; the registry in tests/registry names it.
run-default: rach2 test-image $(RUNTESTS)
	$(RUNTESTS) $(NIC_DEV) --kernel build/rach2/kernel --initrd $(TEST_IMAGE) \
	    --timeout 300 --steptmo 60 --log $(IMAGES_DIR)/default-test.log
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
# image rule names the programs it stages as prerequisites and the component
# that builds them as an order-only one, and make reads a prerequisite's time
# when it first meets the name: before the component's sub-make has run, if
# both are met in one rule. Make then compares the image's time with the old
# one, leaves the image as it was, and a suite boots a program that is not
# the one just compiled. Two Makes read the times twice.
components: harness-kernel busybox check-busybox check-dropbearmulti check-libc \
            check-pdpmake check-setid check-tcc ish krffs musl-programs \
            ports/doom rach2 rach2-metal rach2-recovery tests utils
# The install sticks, named here because `prepare-images` below names them and
# make expands a prerequisite when it reads the rule. Their own rules are with
# the suite that reads them.
# The stick takes the recovery ISO's name with it: a stick whose recovery
# answers this tree's key is not the stick anybody else runs.
INSTALL_STICK = $(IMAGES_DIR)/rach2-install-stick$(RECOVERY_KEY_TAG).img
INSTALL_TEST_STICK = $(IMAGES_DIR)/install-test-stick.img

prepare-images: $(DEMO_CREDS) $(BINFIND) $(DEADLINE) $(ELFCHECK) $(HARNESS) $(HOSTPEER) \
                $(MKFIXTURE) $(MKGPT) $(MKMBR) $(PCAPCHECK) $(PTYDRIVE) $(RUNTESTS) $(RUNUTIL) \
                $(SHA256TOOL) $(WAVCHECK) $(GROWSET) os-real-image os-simple-image \
                rach2-metal-image rach2-recovery-image $(SEALED_DISK) test-image \
                $(INSTALL_STICK) $(INSTALL_TEST_STICK)
prepare-all: components
	$(MAKE) --no-print-directory prepare-images prepare-products
# `tests/run addressed`: the address named on the boot line, and the proof that
# it is the one in force. The address is deliberately not the one SLIRP hands
# out: a kernel that ignored `rach2.ip=` entirely would still hold 10.0.2.15 and
# still pass every probe, which is what this suite used to do with all of them.
# It is not .99 either, which pingtest requires to stay unreachable.
# So the machine is given .77, pings the gateway, and the capture is read from
# outside: the echo request must leave from .77, which only the IP layer can put
# there, and the ARP request for the gateway must be sent as .77, which is where
# the address reached ARP. Nothing on this network ever asks the machine for its
# address, so what it answers cannot be read; what it asks as can. The gateway
# half of the boot line is not checked here: every destination in reach is on
# the subnet, so nothing has to be routed. `dhcp` and `netif` check routing.
ADDRESSED_PCAP = $(IMAGES_DIR)/addressed$(NIC_TAG).pcap
run-addressed: rach2 tests krffs test-image $(PCAPCHECK) $(HARNESS)
	@rm -f $(ADDRESSED_PCAP)
	$(HARNESS) $(NIC_DEV) --kernel build/rach2/kernel \
	    --initrd $(TEST_IMAGE) --qemu-arg=-cpu --qemu-arg=max \
	    --append "rach2.ip=10.0.2.77/24 rach2.gateway=10.0.2.2" \
	    --qemu-arg=-object \
	    --qemu-arg=filter-dump,id=ad0,netdev=net0,file=$(ADDRESSED_PCAP) \
	    --timeout 120 --ok "PINGTEST OK" \
	    "expect=Type 'help'" "send=exec /pingtest" until=60
	$(PCAPCHECK) bcast $(ADDRESSED_PCAP) --source 10.0.2.77 --arp-from 10.0.2.77
# `tests/run noentropy`: the salt policy's negative half, which no other target
# can reach. `make test` always boots -cpu max, so the CPU has RDSEED and RDRAND
# and the kernel's pool is trusted; a minter that quietly fell back to the
# untrusted stream would pass every proof in the suite. This boots the same test
# image on a CPU that exposes neither instruction, and /musl/saltnoent (staged
# by the image sweep, deliberately absent from the probe runner's list because
# it must fail where entropy is trusted) asserts that the mint refuses, says so
# once, and leaves /etc/shadow, its temporary file and the lock exactly as they
# were. The harness passes no -cpu of its own, so the passthrough is the only
# one on the command line.
run-noentropy: rach2 test-image $(HARNESS)
	$(HARNESS) --kernel build/rach2/kernel --initrd $(TEST_IMAGE) \
	    --qemu-arg=-cpu --qemu-arg=qemu64 \
	    --timeout 90 --ok "SALTNOENT OK" \
	    "expect=Type 'help'" "send=exec /musl/saltnoent" until=20
	$(HARNESS) --kernel build/rach2/kernel --initrd $(TEST_IMAGE) \
	    --qemu-arg=-cpu --qemu-arg=qemu64 \
	    --timeout 180 --ok "PASSWDMUSL OK (fail-closed)" \
	    "expect=Type 'help'" "send=exec /musl/passwdmusl" until=60
# `tests/run fpu`: the floating-point state of a process is its own. Two
# boots of `fpuprobe`: with FXSR, where the x87 and XMM registers, the control
# word and MXCSR are saved by `fxsave`, and with `rach2.nofxsr`, where the x87
# state is saved by `fnsave` and an SSE instruction is an invalid opcode, so
# the probe's x87 branch runs alone.
run-fpu: rach2 tests krffs test-image $(HARNESS)
	$(HARNESS) --kernel build/rach2/kernel --initrd $(TEST_IMAGE) \
	    --qemu-arg=-cpu --qemu-arg=max --append "rach2.rescue" \
	    --timeout 90 --steptmo 30 --ok "FPUPROBE OK" --never "FPUPROBE FAIL" \
	    --log $(IMAGES_DIR)/fpu-test.log \
	    "expect=fpu: fxsr: on" "expect=Type 'help'" "send=exec /fpuprobe" "expect=FPUPROBE"
	$(HARNESS) --kernel build/rach2/kernel --initrd $(TEST_IMAGE) \
	    --qemu-arg=-cpu --qemu-arg=max --append "rach2.rescue rach2.nofxsr" \
	    --timeout 90 --steptmo 30 --ok "FPUPROBE OK" --never "FPUPROBE FAIL" \
	    --log $(IMAGES_DIR)/fpu-test.x87.log \
	    "expect=fpu: fxsr: off" "expect=Type 'help'" "send=exec /fpuprobe x87" "expect=FPUPROBE"
# `tests/run memory`: the kernel's account of physical memory at five sizes
# (see tests/system/suites/memory.sh).
run-memory: rach2 rach2-recovery tests krffs test-image $(RECOVERY_IMAGE) $(HARNESS)
	sh tests/system/suites/memory.sh build/rach2/kernel $(TEST_IMAGE) $(HARNESS) \
	    $(RECOVERY_KERNEL) $(RECOVERY_IMAGE)
# `tests/run pools`: the objects that were pools, past the count each pool
# had, and the heap where it started after a thousand of each (see
# tests/poolprobe.c).
run-pools: rach2 tests krffs test-image $(HARNESS)
	$(HARNESS) --kernel build/rach2/kernel --initrd $(TEST_IMAGE) \
	    --qemu-arg=-cpu --qemu-arg=max --append "rach2.rescue" \
	    --timeout 120 --steptmo 60 --ok "POOLPROBE OK" --never "POOLPROBE FAIL" \
	    --log $(IMAGES_DIR)/pools-test.log \
	    "expect=Type 'help'" "send=exec /poolprobe" "expect=POOLPROBE"
# `tests/run fault`: the three probes whose whole claim is that the kernel
# killed them. vmfault writes through a pointer in no mapping, vmro writes a
# page it mapped read-only, and winprobe reads the page-cache window, which
# belongs to the kernel. Each must die on signal 11 and leave the machine
# running, so the boot ends by executing a program that prints a token: a
# kernel that took any of the three faults with it cannot print it. The
# `--never` text is the line each probe prints when it was not killed. These
# cannot join
# the shared probe boot, which scores a probe by its exit status; a probe that
# is killed has none.
run-fault: rach2 tests krffs test-image $(HARNESS)
	$(HARNESS) --kernel build/rach2/kernel --initrd $(TEST_IMAGE) \
	    --qemu-arg=-cpu --qemu-arg=max \
	    --timeout 120 --ok "Hello from ELF!" --never "ERROR still alive" \
	    "expect=Type 'help'" \
	    "send=exec /vmfault" \
	    "expect=vmfault: about to dereference a wild pointer" \
	    "expect=killed by signal 11" \
	    "send=exec /vmro" \
	    "expect=vmro: about to write a read-only page" \
	    "expect=killed by signal 11" \
	    "send=exec /winprobe" \
	    "expect=winprobe: about to read the page-cache window" \
	    "expect=killed by signal 11" \
	    "send=exec /hello"
# `tests/run console`: the two probes that read from the console, which the
# shared probe boot cannot run because they block until something is typed.
# stdintest reads three lines and prints what it read, so the steps assert the
# text as well as the count: a read that returns the right number of the wrong
# bytes passes the probe and fails here. The third read asks for five bytes of a
# longer line, which is where a kernel that ignored the buffer size would be
# caught. ttyprobe is driven the way a terminal drives: a line with two erases
# in it while the console is cooked, then one byte with no newline once the
# probe has switched the console to raw. `sendraw` is what delivers both.
run-console: rach2 tests krffs test-image $(HARNESS)
	$(HARNESS) --kernel build/rach2/kernel --initrd $(TEST_IMAGE) \
	    --qemu-arg=-cpu --qemu-arg=max \
	    --timeout 120 --ok "All stdin syscalls passed!" \
	    "expect=Type 'help'" "send=exec /stdintest" \
	    "expect=Test 1: read from stdin" "send=alpha" \
	    'expect=Read 5 bytes: "alpha"' \
	    "expect=Test 2: second read" "send=bravo line" \
	    'expect=Read 10 bytes: "bravo line"' \
	    "expect=Test 3: small buffer read" "send=charlie-delta" \
	    'expect=Read 5 bytes: "charl"'
	$(HARNESS) --kernel build/rach2/kernel --initrd $(TEST_IMAGE) \
	    --qemu-arg=-cpu --qemu-arg=max \
	    --timeout 120 --ok "TTYPROBE OK" --never "TTYPROBE FAIL" \
	    "expect=Type 'help'" "send=exec /ttyprobe" \
	    "expect=TTYPROBE START" \
	    'sendraw=ab\x7f\x7fcd\n' \
	    "expect=COOKED:" "expect=TTYPROBE RAW" \
	    'sendraw=X' \
	    "expect=RAW:"
# `tests/run audio`: what the machine actually played. The debug shell's
# `audio test scale` plays five notes, and the driver's own report says what it
# was asked to play, not what came out; QEMU's wav backend writes every sample
# the guest produced, so the notes can be measured afterwards from outside.
# The PC-speaker fallback cannot play samples, so the boot names an Intel HDA
# controller and a codec for it to find. The probe reads `/proc/interrupts` on
# the machine before the notes and again after them, and the count on the
# audio controller's message vector must have grown between the two: the notes
# come out either way, since the timer services the stream as well, and the
# difference is what says the interrupt this milestone gave it is the one that
# fed them.
AUDIO_WAV = $(IMAGES_DIR)/audio-scale.wav
AUDIO_LOG = $(IMAGES_DIR)/audio-scale.log
run-audio: rach2 tests krffs test-image $(HARNESS) $(WAVCHECK)
	@rm -f $(AUDIO_WAV) $(AUDIO_LOG)
	$(HARNESS) --kernel build/rach2/kernel --initrd $(TEST_IMAGE) \
	    --qemu-arg=-cpu --qemu-arg=max \
	    --qemu-arg=-device --qemu-arg=intel-hda \
	    --qemu-arg=-device --qemu-arg=hda-duplex,audiodev=au0 \
	    --qemu-arg=-audiodev --qemu-arg=wav,id=au0,path=$(AUDIO_WAV) \
	    --log $(AUDIO_LOG) \
	    --timeout 120 --ok "INTRPROBE OK" --never "INTRPROBE FAIL" \
	    "expect=Type 'help'" "send=exec /intrprobe" "expect=INTRPROBE OK" \
	    "expect=Type 'help'" "send=audio test scale 2000" "expect=Done." \
	    "send=exec /intrprobe" "expect=INTRPROBE OK"
	$(WAVCHECK) scale $(AUDIO_WAV) 262 294 330 349 392
	@awk '/INTRPROBE READ/ { \
	        for (i = 1; i <= NF; i++) { \
	            if ($$i == "audio") { seen++; if (seen == 1) { before = $$(i + 1) + 0; } else { after = $$(i + 1) + 0; } } \
	        } \
	    } \
	    END { \
	        if (seen < 2) { print "audio-test: the probe did not read /proc/interrupts twice" > "/dev/stderr"; exit 1; } \
	        if (after <= before) { print "audio-test: the audio controller sent no message while it played (" before " then " after ")" > "/dev/stderr"; exit 1; } \
	        print "audio-test: the audio controller took " after - before " interrupts on its message vector while it played: OK"; \
	    }' $(AUDIO_LOG)
# `tests/run diskfull`: a file system with no room left. The probe writes until
# the write is refused, requires `ENOSPC` rather than the `EIO` this kernel used
# to return for a full disk, removes the file and writes again. It has a boot of
# its own because it fills the image every other probe is staged in.
run-diskfull: rach2 tests krffs test-image $(HARNESS)
	$(HARNESS) --kernel build/rach2/kernel --initrd $(TEST_IMAGE) \
	    --qemu-arg=-cpu --qemu-arg=max \
	    --timeout 180 --ok "DISKFULL OK" --never "DISKFULL FAIL" \
	    "expect=Type 'help'" "send=exec /diskfull" "expect=DISKFULL"
# `tests/run socket`: the guest opening connections of its own, checked by a
# host that answers (see tests/system/suites/socket.sh).
run-socket: rach2 tests krffs test-image $(HARNESS) $(HOSTPEER) $(RUNUTIL)
	sh tests/system/suites/socket.sh build/rach2/kernel $(TEST_IMAGE) \
	    $(HARNESS) $(HOSTPEER)
# `tests/run disk`: the disk-backed page-cache integrity check. Builds a disk
# image holding blockprobe, boots disk-only with KRFFS on the real ATA disk
# through the cache (rach2.fsdisk), and runs blockprobe from the debug shell,
# passing on its "BLOCKPROBE OK" serial token. blockprobe writes more than the
# resident cache, so it exercises real eviction and writeback and fails on any
# dropped or corrupted page.
DISK_TEST_IMAGE = $(IMAGES_DIR)/disk-test.disk
run-disk: rach2 tests krffs $(HARNESS)
	$(MKDISK) krffs/manifests/disk-test.manifest $(DISK_TEST_IMAGE) 64 48
	$(HARNESS) --kernel build/rach2/kernel \
	    --append "rach2.fsdisk rach2.rescue" \
	    --drive "file=$(DISK_TEST_IMAGE),format=raw,if=ide,index=0,media=disk" \
	    --timeout 120 --steptmo 90 --ok "BLOCKPROBE OK" --log $(DISK_TEST_IMAGE).log probe=blockprobe
# `tests/run bootpart`: /proc/cmdline and the level-one disk devices. Three
# boots of one disk: as mkdisk built it there is a single KRFFS partition, so
# /dev/bootpart must be absent from stat and open; after the boot fixture adds a
# FAT-typed partition of known bytes, the probe drives the device end to end
# (the 0600 mode from both stat paths, seeks against the partition size, known
# bytes at a seek, a written sector read back, exactly 0 at the end, -ENOSPC
# past it, -EINVAL unaligned, -EACCES unprivileged); and a third boot mounts a
# RAM image instead of the disk and requires both /dev/bootpart and
# /dev/grubcore anyway. That third one is the milestone 25 property: a machine
# that mounts nothing from its disk still manages its boot partition and its
# boot loader, which is what recovery depends on. A fourth boot attaches a
# sparse 260 GB drive over AHCI and reads known sectors at 5 GiB and at 258 GiB,
# which the raw windows could not address at all while an open file's offset was
# 32 bits. It is that large on purpose: a few gigabytes would be satisfied by an
# implementation that kept 33 bits, and it is attached over AHCI because the ATA
# driver's 28-bit LBA stops at 128 GiB.
BOOTPART_TEST_IMAGE = $(IMAGES_DIR)/bootpart-test.disk
WIDE_TEST_IMAGE = $(IMAGES_DIR)/wide-test.disk
run-bootpart: rach2 tests krffs test-image $(MKFIXTURE) $(HARNESS)
	$(MKDISK) krffs/manifests/disk-test.manifest $(BOOTPART_TEST_IMAGE) 64 48
	$(HARNESS) --kernel build/rach2/kernel \
	    --append "rach2.fsdisk rach2.rescue" \
	    --drive "file=$(BOOTPART_TEST_IMAGE),format=raw,if=ide,index=0,media=disk" \
	    --timeout 90 --ok "BOOTPROBE ABSENT OK" probe=bootprobe
	$(MKFIXTURE) boot $(BOOTPART_TEST_IMAGE)
	$(HARNESS) --kernel build/rach2/kernel \
	    --append "rach2.fsdisk rach2.rescue rach2.bootprobe=fixture" \
	    --drive "file=$(BOOTPART_TEST_IMAGE),format=raw,if=ide,index=0,media=disk" \
	    --timeout 90 --ok "BOOTPROBE OK" probe=bootprobe
	$(HARNESS) --kernel build/rach2/kernel --initrd $(TEST_IMAGE) \
	    --append "rach2.rescue rach2.bootprobe=ramroot" \
	    --drive "file=$(BOOTPART_TEST_IMAGE),format=raw,if=ide,index=0,media=disk" \
	    --timeout 90 --ok "BOOTPROBE RAMROOT OK" probe=bootprobe
	$(MKFIXTURE) wide $(WIDE_TEST_IMAGE)
	$(HARNESS) --kernel build/rach2/kernel --initrd $(TEST_IMAGE) \
	    --append "rach2.disk=ahci rach2.rescue rach2.diskdiag rach2.bootprobe=wide" \
	    --drive "file=$(WIDE_TEST_IMAGE),format=raw,if=none,id=drv0" \
	    $(AHCI_ATTACH) \
	    --timeout 180 --ok "BOOTPROBE WIDE OK" probe=bootprobe
# `tests/run grub`: the machine replacing its own boot loader. The post-MBR gap
# holds two core-image tails and LBA 1 selects between them, so a replacement
# writes the tail not in use, reads it back, and only then rewrites that one
# sector. Sector 0, which carries the partition table, is never written.
#
# Two core images are built with a marker embedded in each and staged into the
# root through os-real's optional-file lines, then installed in turn; every
# boot's marker must be the image the boot before it installed. The marker comes
# from inside the running core, because reading the pointer back would prove
# only bytes at rest.
GRUB_TEST_DISK = $(IMAGES_DIR)/grub-test.disk
# The root this disk boots. It was a KRFFS partition carrying grub-test.manifest
# until 2026-09-14; the same manifest, as the ext2 root the disk now has, so the
# guest commands this suite types still find what they expect.
# The two marked cores the root's manifest stages. They were made inside the
# `run-grub` recipe, which ran after the root was staged, so a clean build
# staged nothing and a used tree staged the previous pair: rules of their own,
# and the root waits for them.
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
$(GRUB_TEST_EXT2): $(DEMO_CREDS) $(GRUB_CORE_A) $(GRUB_CORE_B) krffs/manifests/grub-test.manifest $(MANIFEST_DEPS_grub_test) tools/image/mkext2.sh $(IMAGE_TOOLS) | preflight krffs rach2 utils check-setid
	sh tools/image/mkext2.sh krffs/manifests/grub-test.manifest $@ 48
run-grub: $(DEMO_CREDS) rach2 rach2-recovery rach2-recovery-image utils krffs $(GRUB_TEST_EXT2) check-busybox check-tcc check-pdpmake check-setid check-dropbearmulti ports/doom $(RUNUTIL)
	@mkdir -p $(IMAGES_DIR)
	sed -e '/^#/d' -e '/^$$/d' -e '\|^file 0644 /core-[ab]\.img|d' \
	    krffs/manifests/grub-test.manifest > $(IMAGES_DIR)/grub-test.manifest.stripped
	sed -e '/^#/d' -e '/^$$/d' krffs/manifests/os-real.manifest \
	    | diff - $(IMAGES_DIR)/grub-test.manifest.stripped
	RACH2_DISK_EXT2_MB=48 \
	RACH2_DISK_EXT2_IMAGE=$(GRUB_TEST_EXT2) \
	RACH2_DISK_RECOVERY_MB=8 \
	    sh tools/image/mkdisk_grub.sh build/rach2/kernel $(GRUB_TEST_DISK)
	sh tests/system/suites/grub.sh $(GRUB_TEST_DISK) \
	    "`cat krffs/rach2_demo_password`"
# `tests/run bootorder`: the GRUB disk on the second AHCI port behind a
# disk holding nothing, booted by the firmware, its root found as disk 1.
BOOTORDER_DISK = $(IMAGES_DIR)/bootorder-test.disk
run-bootorder: $(DEMO_CREDS) rach2 rach2-recovery rach2-recovery-image utils krffs $(GRUB_TEST_EXT2) check-busybox check-tcc check-pdpmake check-setid check-dropbearmulti ports/doom $(HARNESS)
	sh tests/system/suites/bootorder.sh build/rach2/kernel $(HARNESS) $(BOOTORDER_DISK)
# `tests/run menu`: the boot menu that can be rewritten, and the stub that
# cannot. Four boots of one GRUB disk: a two-entry menu staged through `bootctl
# config` with a grubenv key round-tripped and the candidate kernel corrupted; a
# boot proving both staged entries reached the menu and that the fallback NUMBER
# still named the stub's own entry rather than a staged one; that file truncated
# mid-entry from the host, after which the machine must still boot through the
# stub with GRUB's parse error in the log; and the slot's extent read back to
# prove bootctl overwrote bytes in place rather than rewriting a file.
MENU_TEST_DISK = $(IMAGES_DIR)/menu-test.disk
# The root is os-real's, not os-simple's, because every line this suite types
# at the machine is a busybox command and os-simple carries no busybox: with
# that root the first command printed nothing and the suite waited out its
# deadline.
run-menu: $(DEMO_CREDS) rach2 utils krffs $(OS_REAL_SMALL_EXT2) check-busybox check-tcc check-pdpmake check-setid check-dropbearmulti ports/doom $(BINFIND) $(RUNUTIL)
	@mkdir -p $(IMAGES_DIR)
	RACH2_DISK_RECOVERY_MB=8 \
	RACH2_DISK_EXT2_MB=96 \
	RACH2_DISK_EXT2_IMAGE=$(OS_REAL_SMALL_EXT2) \
	    sh tools/image/mkdisk_grub.sh build/rach2/kernel $(MENU_TEST_DISK)
	sh tests/system/suites/menu.sh $(MENU_TEST_DISK) \
	    "`cat krffs/rach2_demo_password`"
# `tests/run authority`: which kernel may write the whole disk. Two boots of one
# probe against one sparse image, differing only in the kernel. The everyday
# build must refuse a write to /dev/disk0 with -EPERM and the sector must still
# be zero afterwards, so the refusal is proved to have written nothing rather
# than merely returned an error. The recovery build must write a sector past 2
# GiB, read it back byte for byte, and take a 200-sector write in one call,
# which is what makes a whole-disk reflash finishable: one sector per driver
# call turned a 500 MiB write into a million cache flushes.
#
# The two kernels are the same sources built with different switches into
# different object trees, and check_recovery.sh reads the authority back off
# each artifact so a mislabelled one cannot reach this test.
AUTH_TEST_IMAGE = $(IMAGES_DIR)/auth-test.disk
run-authority: rach2 rach2-recovery tests krffs test-image $(MKFIXTURE) $(HARNESS)
	$(MKFIXTURE) wide $(AUTH_TEST_IMAGE)
	sh tools/inspect/check_recovery.sh build/rach2/kernel ordinary
	sh tools/inspect/check_recovery.sh $(RECOVERY_KERNEL) recovery
	$(HARNESS) --kernel build/rach2/kernel --initrd $(TEST_IMAGE) \
	    --append "rach2.disk=ahci rach2.rescue rach2.diskdiag rach2.bootprobe=auth-ordinary" \
	    --drive "file=$(AUTH_TEST_IMAGE),format=raw,if=none,id=drv0" \
	    $(AHCI_ATTACH) \
	    --timeout 180 --ok "BOOTPROBE AUTH ORDINARY OK" probe=bootprobe
	$(HARNESS) --kernel $(RECOVERY_KERNEL) --initrd $(TEST_IMAGE) \
	    --append "rach2.disk=ahci rach2.rescue rach2.diskdiag rach2.bootprobe=auth-recovery" \
	    --drive "file=$(AUTH_TEST_IMAGE),format=raw,if=none,id=drv0" \
	    $(AHCI_ATTACH) \
	    --timeout 180 --ok "BOOTPROBE AUTH RECOVERY OK" probe=bootprobe
# `tests/run install`: the machine installs its own kernel. Builds the GRUB disk
# around the host kernel, boots it with no -kernel, and runs the whole install
# loop twice as root: a marker edited into the banner source, the kernel built
# in place, installed as a one-boot trial, proved running by its marker after
# reboot -f, committed, held across a cold boot, corrupted at the
# installer-named extent, recovered through the fallback and repaired from
# inside. The interruption matrix's determinate states are then booted from
# host-crafted environment blocks.
# `tests/run install`: the suite on a disk whose entries boot the ext2 root,
# the os-real system at 96 MiB. The kernel is built on that root and installed
# from it. It ran twice until 2026-09-14, once per root type.
OS_REAL_SMALL_EXT2 = $(IMAGES_DIR)/os-real-small.ext2
$(OS_REAL_SMALL_EXT2): $(DEMO_CREDS) krffs/manifests/os-real.manifest $(MANIFEST_DEPS_os_real) tools/image/mkext2.sh $(IMAGE_TOOLS) | preflight krffs rach2 utils check-busybox check-tcc check-pdpmake check-setid check-dropbearmulti ports/doom
	sh tools/image/mkext2.sh krffs/manifests/os-real.manifest $@ 96
INSTALL_TEST_DISK = $(IMAGES_DIR)/install-test.disk
run-install: $(DEMO_CREDS) rach2 utils krffs check-busybox check-tcc check-pdpmake check-setid check-dropbearmulti ports/doom $(BINFIND) rach2-recovery $(RECOVERY_IMAGE) $(OS_REAL_SMALL_EXT2) $(RUNUTIL)
	@mkdir -p $(IMAGES_DIR)
	RACH2_DISK_RECOVERY_MB=8 \
	RACH2_DISK_EXT2_MB=96 \
	RACH2_DISK_EXT2_IMAGE=$(OS_REAL_SMALL_EXT2) \
	RACH2_DISK_RECOVERY_KERNEL=$(RECOVERY_KERNEL) \
	RACH2_DISK_RECOVERY_IMAGE=$(RECOVERY_IMAGE) \
	    sh tools/image/mkdisk_grub.sh build/rach2/kernel $(INSTALL_TEST_DISK)
	sh tests/system/suites/install.sh $(INSTALL_TEST_DISK) \
	    "`cat krffs/rach2_demo_password`"
# `tests/run sealed`: the disk that goes into the laptop, operated the way it
# will be operated once it is in there. Two runs of one artifact on separate
# copies: a fault run that walks the whole life of the machine over SSH (a
# kernel sent down the connection, installed, tried and committed; a destroyed
# candidate caught by the fallback and repaired; a menu staged and then torn;
# the boot loader replaced with a marked core; the root destroyed and rewritten
# from recovery), and a control run that injects nothing and asserts that no
# fallback and no recovery appears anywhere. The disk is assembled natively; the
# runs are QEMU and ssh.
# The disk itself and not the alias that builds it: the alias is a phony whose
# recipe always runs, and two suites of the pool that named it would write the
# one image at the same time. `prepare-images` builds the file before the pool
# starts, and a suite run on its own builds it through this rule.
run-sealed: $(DEMO_CREDS) $(SEALED_DISK) $(DEADLINE) $(ELFCHECK) $(BINFIND) $(MKMBR) $(RUNUTIL)
	sh tests/system/suites/sealed.sh $(SEALED_DISK) \
	    "`cat krffs/rach2_demo_password`" $(SEALED_PORT)
# `tests/run drill`: the recovery drill, boots of one disk that carries the same
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
# `tests/run drill`: the root the everyday kernel must refuse to write, that is
# destroyed, that recovery rewrites through the pipe, and that the last boot
# mounts again. It ran twice until 2026-09-14, once per root type; the disk has
# one root now.
OS_SIMPLE_EXT2 = $(IMAGES_DIR)/os-simple.ext2
$(OS_SIMPLE_EXT2): $(DEMO_CREDS) krffs/manifests/os-simple.manifest $(MANIFEST_DEPS_os_simple) tools/image/mkext2.sh $(IMAGE_TOOLS) | preflight krffs rach2 utils check-setid
	sh tools/image/mkext2.sh krffs/manifests/os-simple.manifest $@ 16
DRILL_DISK = $(IMAGES_DIR)/drill-test.disk
run-drill: $(DEMO_CREDS) rach2 rach2-recovery rach2-recovery-image utils krffs check-setid $(BINFIND) $(MKMBR) $(HARNESS) $(OS_SIMPLE_EXT2) $(RUNUTIL)
	@mkdir -p $(IMAGES_DIR)
	RACH2_DISK_RECOVERY_MB=8 \
	RACH2_DISK_EXT2_MB=16 \
	RACH2_DISK_EXT2_IMAGE=$(OS_SIMPLE_EXT2) \
	RACH2_DISK_APPEND="rach2.diskdiag" \
	RACH2_DISK_DHCP_APPEND="rach2.ip=none" \
	RACH2_DISK_RECOVERY_KERNEL=$(RECOVERY_KERNEL) \
	RACH2_DISK_RECOVERY_IMAGE=$(RECOVERY_IMAGE) \
	    sh tools/image/mkdisk_grub.sh build/rach2/kernel $(DRILL_DISK)
	sh tests/system/suites/drill.sh $(DRILL_DISK) \
	    "`cat krffs/rach2_demo_password`" $(DRILL_PORT) $(RECOVERY_IMAGE)
# `tests/run persist`: fsync durability across a reboot. Builds a disk holding
# persistprobe, boots it twice against the SAME image: the first boot writes and
# fsyncs /persist.txt, the second reads it back. The second boot passes only if
# fsync pushed the write through the cache to the disk, since the file would
# otherwise be gone.
PERSIST_DISK = $(IMAGES_DIR)/persist-test.disk
run-persist: rach2 tests krffs $(HARNESS)
	$(MKDISK) krffs/manifests/persist-test.manifest $(PERSIST_DISK) 64 48
	$(HARNESS) --kernel build/rach2/kernel \
	    --append "rach2.fsdisk rach2.rescue" \
	    --drive "file=$(PERSIST_DISK),format=raw,if=ide,index=0,media=disk" \
	    --timeout 60 --ok "PERSISTPROBE OK (wrote)" probe=persistprobe
	$(HARNESS) --kernel build/rach2/kernel \
	    --append "rach2.fsdisk rach2.rescue" \
	    --drive "file=$(PERSIST_DISK),format=raw,if=ide,index=0,media=disk" \
	    --timeout 60 --ok "PERSISTPROBE OK (read back)" probe=persistprobe
# `tests/run cred-persist`: the credential replacement's durability across a
# reboot. Both of cred_rewrite's flushes are no-ops on the RAM module `make
# test` boots, so a library that dropped either would pass every other proof in
# the milestone. This boots one disk image three times. The first boot publishes
# one replacement and releases, then publishes a second and is cut off holding
# the lock, as a power failure would cut it off. The second boot reads both back
# with the modes and owners they inherited, and reports the lock the cut-off
# writer left rather than breaking it. The third reads back the release itself:
# a lock whose removal stayed in the page cache would still be there.
CRED_PERSIST_DISK = $(IMAGES_DIR)/cred-persist-test.disk
run-cred-persist: rach2 musl-programs krffs $(HARNESS)
	$(MKDISK) krffs/manifests/cred-persist-test.manifest $(CRED_PERSIST_DISK) 64 48
	$(HARNESS) --kernel build/rach2/kernel \
	    --append "rach2.fsdisk rach2.rescue" \
	    --drive "file=$(CRED_PERSIST_DISK),format=raw,if=ide,index=0,media=disk" \
	    --timeout 60 --ok "CREDDISK OK (wrote)" \
	    "expect=Type 'help'" "send=exec /creddisk" until=15
	$(HARNESS) --kernel build/rach2/kernel \
	    --append "rach2.fsdisk rach2.rescue" \
	    --drive "file=$(CRED_PERSIST_DISK),format=raw,if=ide,index=0,media=disk" \
	    --timeout 60 --ok "CREDDISK OK (released)" \
	    "expect=Type 'help'" "send=exec /creddisk" until=15
	$(HARNESS) --kernel build/rach2/kernel \
	    --append "rach2.fsdisk rach2.rescue" \
	    --drive "file=$(CRED_PERSIST_DISK),format=raw,if=ide,index=0,media=disk" \
	    --timeout 60 --ok "CREDDISK OK (read back)" \
	    "expect=Type 'help'" "send=exec /creddisk" until=15
# `tests/run crash`: prove the dirty-mount flag detects an unclean shutdown. A
# crashed boot (killed after mounting) leaves the on-disk dirty bit set for
# fsck.krffs to report; a clean halt clears it. Its own image so it cannot race
# `tests/run disk` on a shared file under `make -j`.
CRASH_TEST_IMAGE = $(IMAGES_DIR)/crash-test.disk
# The capture `tests/run raw` reads the hop limits and the checksum out of.
RAW_PCAP = $(IMAGES_DIR)/raw$(NIC_TAG).pcap
# Its own disk image, so `make -j` cannot have this target and `test-disk`
# rewriting one file while the other boots it.
BOOTLINE_TEST_IMAGE = $(IMAGES_DIR)/bootline-test.disk
# `tests/run ping`: the ICMP path against the SLIRP gateway, which is why it is
# its own target rather than a probe in the suite. The explicit until= is
# required: after its last step the harness drains only --grace and then kills
# QEMU, while the timeout case alone costs four seconds and the child the probe
# forks lives three to eight, so without it the token would arrive after QEMU
# was dead.
run-ping: rach2 tests krffs test-image $(HARNESS)
	$(HARNESS) $(NIC_DEV) --kernel build/rach2/kernel \
	    --initrd $(TEST_IMAGE) --qemu-arg=-cpu --qemu-arg=max \
	    --timeout 120 --ok "PINGTEST OK" \
	    "expect=Type 'help'" "send=exec /pingtest" until=60
# `tests/run packet`: the packet socket, which is how a DHCP client sees a reply
# that arrives before this machine owns an address. Its own target and not a
# probe in the suite, for the reason `tests/run ping` is: it needs the
# network to answer, and the suite's probes must not.
run-packet: rach2 tests krffs test-image $(HARNESS)
	$(HARNESS) $(NIC_DEV) --kernel build/rach2/kernel \
	    --initrd $(TEST_IMAGE) --qemu-arg=-cpu --qemu-arg=max \
	    --timeout 120 --ok "PACKETPROBE OK" \
	    "expect=Type 'help'" "send=exec /packetprobe" until=60
# `tests/run bootline`: the boot line that contradicts itself. `rach2.nodisk`
# skips the disk an image rooted on it needs, and the machine would fall to a
# shell that authenticates nobody, so the kernel refuses that combination; with
# `rach2.rescue` it is allowed, since asking for that shell is what the token is
# for. Four boots, and each refusal is asserted twice: the diagnostic, and that
# no shell was reached after it.
run-bootline: rach2 tests krffs $(RUNUTIL)
	$(MKDISK) krffs/manifests/disk-test.manifest $(BOOTLINE_TEST_IMAGE) 64 48
	sh tests/system/suites/bootline.sh build/rach2/kernel $(BOOTLINE_TEST_IMAGE)
# `tests/run raw`: the raw ICMP socket, which is what `ping` and `traceroute`
# are. Three boots, because the three things it must show cannot be shown in
# one: the probe with the network answering, the same probe with an address and
# no gateway (where the only truthful answer is that there is no route), and
# `traceroute` itself on the image that has it. The capture is read by the
# capture checker for the two things a program inside the machine cannot see:
# the hop limit each datagram left with, and the caller's own checksum.
run-raw: raw-stack
raw-card: $(DEMO_CREDS) rach2 tests krffs test-image os-real-image $(PCAPCHECK) $(HARNESS)
	@rm -f $(RAW_PCAP)
	$(HARNESS) $(NIC_DEV) --kernel build/rach2/kernel \
	    --initrd $(TEST_IMAGE) --qemu-arg=-cpu --qemu-arg=max \
	    --qemu-arg=-object \
	    --qemu-arg=filter-dump,id=rw0,netdev=net0,file=$(RAW_PCAP) \
	    --timeout 200 --ok "RAWPROBE OK" --log $(IMAGES_DIR)/raw-test.rawprobe.log \
	    "expect=Type 'help'" "send=exec /rawprobe" until=60
	$(PCAPCHECK) rawwire $(RAW_PCAP)
ifeq ($(NIC),e1000)
raw-stack: raw-card
	@echo "raw-test: the rows above the driver, the route that is not there, traceroute and the user's ping, are skipped here; tests/run raw covers them on the first card"
else
raw-stack: raw-card
	$(HARNESS) $(NIC_DEV) --kernel build/rach2/kernel \
	    --initrd $(TEST_IMAGE) --qemu-arg=-cpu --qemu-arg=max \
	    --append "rach2.ip=10.0.2.15/24" \
	    --timeout 200 --ok "RAWPROBE OK" --log $(IMAGES_DIR)/raw-test.static.log \
	    "expect=Type 'help'" "send=exec /rawprobe" until=30
	# And the program the socket was built for. The ping first is not a flourish: a
	# datagram for an address whose hardware address is not known yet is dropped
	# while the request goes out, and traceroute sends one probe per hop and does
	# not retry, so a cold cache costs it its first answer. What is asserted is the
	# first hop, which is the gateway answering a hop limit of one, and that is the
	# whole mechanism.
	$(HARNESS) $(NIC_DEV) --kernel build/rach2/kernel \
	    --initrd $(OS_REAL_IMAGE) --qemu-arg=-cpu --qemu-arg=max \
	    --timeout 300 --steptmo 90 --ok "TRACEDONE" \
	    "expect=rach2 login:" "send=root" "expect=Password:" \
	    "send=`cat krffs/rach2_demo_password`" \
	    "expect=# " \
	    "send=/bin/ping 10.0.2.2 > /dev/null; traceroute -n -q 1 -m 2 8.8.8.8" \
	    "expect= 1  10.0.2.2" "send=echo TRACE\"\"DONE" "expect=TRACEDONE"
	# The in-house ping is still the one an ordinary account runs: busybox's needs
	# a raw socket, which is root's, and is not built for that reason.
	$(HARNESS) $(NIC_DEV) --kernel build/rach2/kernel \
	    --initrd $(OS_REAL_IMAGE) --qemu-arg=-cpu --qemu-arg=max \
	    --timeout 300 --steptmo 90 --ok "USERPINGOK" \
	    "expect=rach2 login:" "send=test" "expect=Password:" \
	    "send=`cat krffs/rach2_test_password`" "expect=[$$] " \
	    "send=/bin/ping 10.0.2.2 && echo USERPING\"\"OK" "expect=USERPINGOK"
endif
.PHONY: raw-card raw-stack
# `tests/run netif`: the interface ioctls, which is how a program sets this
# machine's address. Its own target, like `tests/run ping`: it changes the
# address out from under the machine and puts it back, and the last thing it
# does is ping the gateway, which needs the network to answer.
TWONIC_LOG = $(IMAGES_DIR)/netif-test.twonic$(NIC_TAG).log
MIXNIC_LOG = $(IMAGES_DIR)/netif-test.mixnic$(NIC_TAG).log
NICFAIL_LOG = $(IMAGES_DIR)/netif-test.nicfail$(NIC_TAG).log
run-netif: netif-stack
netif-card: rach2 tests krffs test-image $(HARNESS)
	$(HARNESS) $(NIC_DEV) --kernel build/rach2/kernel \
	    --initrd $(TEST_IMAGE) --qemu-arg=-cpu --qemu-arg=max \
	    --net "user,id=net0,hostfwd=udp::$(NETIF_PORT)-:9999" \
	    --timeout 180 --ok "NETIFPROBE OK" \
	    "expect=Type 'help'" "send=exec /netifprobe" \
	    "expect=NETIFPROBE DOWN" "udp=$(NETIF_PORT):down" \
	    "expect=NETIFPROBE UP" "udp=$(NETIF_PORT):up" until=60
	$(HARNESS) $(NIC_DEV) --kernel build/rach2/kernel \
	    --initrd $(TEST_IMAGE) --qemu-arg=-cpu --qemu-arg=max \
	    --append "rach2.ip=none" \
	    --timeout 120 --ok "NETIFPROBE OK" \
	    "expect=Type 'help'" "send=exec /netifprobe" until=60
ifeq ($(NIC),e1000)
netif-stack: netif-card
	@echo "netif-test: the boots that name their cards, two cards, two models and a refused card, are skipped here; tests/run netif covers them"
else
netif-stack: netif-card
# Two cards on two subnets: each becomes an interface of its own, with its own
# name, its own station address and its own line in the boot report and in
# `/proc/net/dev`. The first still carries the traffic, which is what says the
# second did not take the stack over.
	@rm -f $(TWONIC_LOG)
	$(HARNESS) --kernel build/rach2/kernel --initrd $(TEST_IMAGE) \
	    --qemu-arg=-cpu --qemu-arg=max \
	    --net "user,id=net0" --device "e1000,netdev=net0" \
	    --qemu-arg=-netdev --qemu-arg=user,id=net1,net=10.0.3.0/24 \
	    --qemu-arg=-device --qemu-arg=e1000,netdev=net1 \
	    --append "rach2.ifaddr=52:54:00:12:34:57,10.0.3.5/24,10.0.3.2" \
	    --log $(TWONIC_LOG) \
	    --timeout 180 --steptmo 60 --ok "NETIFPROBE OK" \
	    "expect=Type 'help'" "send=exec /pingtest" "expect=PINGTEST OK" \
	    "expect=Type 'help'" "send=exec /netifprobe second" until=60
	@sh -c 'l=$(TWONIC_LOG); \
	    grep -aq "net: eth0: e1000" "$$l" || \
	        { echo "netif-test: FAIL: the first card did not become eth0" >&2; exit 1; }; \
	    grep -aq "net: eth1: e1000" "$$l" || \
	        { echo "netif-test: FAIL: the second card did not become eth1" >&2; exit 1; }; \
	    a=`sed -n "s/.*net: eth0: e1000, station \\(.*\\)/\\1/p" "$$l" | head -1`; \
	    b=`sed -n "s/.*net: eth1: e1000, station \\(.*\\)/\\1/p" "$$l" | head -1`; \
	    [ -n "$$a" ] && [ "$$a" != "$$b" ] || \
	        { echo "netif-test: FAIL: the two cards report one station address" >&2; exit 1; }; \
	    grep -aq "net: eth1: configured by its station address" "$$l" || \
	        { echo "netif-test: FAIL: the second card was not configured by its station" >&2; exit 1; }; \
	    grep -aq "net: eth1: IPv4 10.0.3.5/24, gateway 10.0.3.2" "$$l" || \
	        { echo "netif-test: FAIL: the second card did not take the address it was given" >&2; exit 1; }; \
	    grep -aq "net: eth0: IPv4 10.0.2.15/24" "$$l" || \
	        { echo "netif-test: FAIL: the first card lost its own address" >&2; exit 1; }; \
	    echo "netif-test: two cards are two interfaces, each with its own station and address: OK"'
# Two cards of different models on two subnets. The driver table tries the
# RTL8139 first, so the RTL8139 is eth0 whichever slot it sits in and the e1000
# is eth1: probe order is the table's, and each card is given its address by the
# station address it carries rather than by where it was found. The ping leaves
# by the second card, which is the one on the network the gateway is on, while
# the first is up and configured on another.
	@rm -f $(MIXNIC_LOG)
	$(HARNESS) --kernel build/rach2/kernel --initrd $(TEST_IMAGE) \
	    --qemu-arg=-cpu --qemu-arg=max \
	    --net "user,id=net0" --device "e1000,netdev=net0,mac=52:54:00:12:34:56" \
	    --qemu-arg=-netdev --qemu-arg=user,id=net1,net=10.0.3.0/24 \
	    --qemu-arg=-device --qemu-arg=rtl8139,netdev=net1,mac=52:54:00:12:34:58 \
	    --append "rach2.ifaddr=52:54:00:12:34:58,10.0.3.5/24,10.0.3.2 \
rach2.ifaddr=52:54:00:12:34:56,10.0.2.15/24,10.0.2.2" \
	    --log $(MIXNIC_LOG) \
	    --timeout 180 --steptmo 60 --ok "PINGTEST OK" \
	    "expect=Type 'help'" "send=exec /pingtest" until=60
	@sh -c 'l=$(MIXNIC_LOG); \
	    grep -aq "net: eth0: RTL8139" "$$l" || \
	        { echo "netif-test: FAIL: the RTL8139 did not become eth0" >&2; exit 1; }; \
	    grep -aq "net: eth1: e1000" "$$l" || \
	        { echo "netif-test: FAIL: the e1000 did not become eth1" >&2; exit 1; }; \
	    grep -aq "net: eth0: IPv4 10.0.3.5/24, gateway 10.0.3.2" "$$l" || \
	        { echo "netif-test: FAIL: the RTL8139 did not take its own address" >&2; exit 1; }; \
	    grep -aq "net: eth1: IPv4 10.0.2.15/24, gateway 10.0.2.2" "$$l" || \
	        { echo "netif-test: FAIL: the e1000 did not take its own address" >&2; exit 1; }; \
	    echo "netif-test: two cards of different models are two interfaces, and the ping \
leaves by the one that reaches the gateway: OK"'
# The first card refused at its bring-up: the second still comes up, takes the
# one interface there is, and carries the traffic.
	@rm -f $(NICFAIL_LOG)
	$(HARNESS) --kernel build/rach2/kernel --initrd $(TEST_IMAGE) \
	    --qemu-arg=-cpu --qemu-arg=max \
	    --net "user,id=net0" --device "e1000,netdev=net0" \
	    --qemu-arg=-netdev --qemu-arg=user,id=net1 \
	    --qemu-arg=-device --qemu-arg=e1000,netdev=net1 \
	    --append "rach2.nicfail=0" \
	    --log $(NICFAIL_LOG) \
	    --timeout 120 --ok "PINGTEST OK" \
	    "expect=Type 'help'" "send=exec /pingtest" until=60
	@sh -c 'l=$(NICFAIL_LOG); \
	    grep -aq "net: eth0: refused: rach2.nicfail=0" "$$l" || \
	        { echo "netif-test: FAIL: the card was not refused" >&2; exit 1; }; \
	    n=`grep -ac "net: eth[0-9]*: e1000, station" "$$l"`; \
	    [ "$$n" = "1" ] || \
	        { echo "netif-test: FAIL: $$n interfaces came up, not one" >&2; exit 1; }; \
	    echo "netif-test: a card refused at its bring-up leaves the other one serving: OK"'
endif
.PHONY: netif-card netif-stack
# `tests/run dgram`: a connected datagram socket and the permission to
# broadcast, which is how a DHCP client renews a lease and how it asks for one.
# Its own target because it broadcasts, and a probe in the suite must not put a
# frame on the wire that the other probes did not ask for.
DGRAM_PCAP = $(IMAGES_DIR)/dgram$(NIC_TAG).pcap
run-dgram: rach2 tests krffs test-image $(PCAPCHECK) $(HARNESS)
	@rm -f $(DGRAM_PCAP)
	$(HARNESS) $(NIC_DEV) --kernel build/rach2/kernel \
	    --initrd $(TEST_IMAGE) --qemu-arg=-cpu --qemu-arg=max \
	    --net "user,id=net0,hostfwd=udp::$(DGRAM_PORT)-:7502" \
	    --qemu-arg=-object \
	    --qemu-arg=filter-dump,id=dg0,netdev=net0,file=$(DGRAM_PCAP) \
	    --timeout 180 --ok "DGRAMPROBE OK" \
	    "expect=Type 'help'" "send=exec /dgramprobe" \
	    "expect=DGRAMPROBE OUTSIDE" "udp=$(DGRAM_PORT):outsider" "expect=DGRAMPROBE OK"
	$(PCAPCHECK) bcast $(DGRAM_PCAP) --port 7500 --dst-ip 255.255.255.255
# `tests/run dhcp`: the machine taking an address it was not told, with the
# client os-real carries. It boots with `rach2.ip=none`, so IPv4 is unset and
# nothing is configured until userland does it, and QEMU's user network is given
# a network of its own so the address that arrives cannot be the one the machine
# would have had anyway.
DHCP_NET = user,id=net0,net=192.168.90.0/24,host=192.168.90.2,dhcpstart=192.168.90.20,dns=192.168.90.3
TWONET_PCAP_A = $(IMAGES_DIR)/twonet-a$(NIC_TAG).pcap
TWONET_PCAP_B = $(IMAGES_DIR)/twonet-b$(NIC_TAG).pcap
DHCP_PCAP = $(IMAGES_DIR)/dhcp$(NIC_TAG).pcap
DHCP_DISK = $(IMAGES_DIR)/dhcp-test$(NIC_TAG).disk
METAL_PCAP = $(IMAGES_DIR)/dhcp-metal$(NIC_TAG).pcap
SIMPLE_PCAP = $(IMAGES_DIR)/dhcp-simple$(NIC_TAG).pcap
SIMPLE_LOG = $(IMAGES_DIR)/dhcp-simple$(NIC_TAG).log
STATIC_PCAP = $(IMAGES_DIR)/static-simple$(NIC_TAG).pcap
RSTATIC_PCAP = $(IMAGES_DIR)/static-real$(NIC_TAG).pcap
SIMPLE_NET = user,id=net0,net=10.90.0.0/16,host=10.90.0.2,dhcpstart=10.90.0.20,dns=10.90.0.3
run-dhcp: dhcp-stack
dhcp-card: $(DEMO_CREDS) rach2 utils busybox krffs os-real-image os-simple-image rach2-metal-image $(PCAPCHECK) $(HARNESS)
	@rm -f $(DHCP_PCAP)
	$(HARNESS) $(NIC_DEV) --kernel build/rach2/kernel \
	    --initrd $(OS_REAL_IMAGE) --qemu-arg=-cpu --qemu-arg=max \
	    --net "$(DHCP_NET)" \
	    --qemu-arg=-object \
	    --qemu-arg=filter-dump,id=dh0,netdev=net0,file=$(DHCP_PCAP) \
	    --append "rach2.ip=none" \
	    --timeout 240 --steptmo 60 --ok "DHCPOK" --log $(IMAGES_DIR)/dhcp$(NIC_TAG).log \
	    "expect=rach2 login:" "send=root" "expect=Password:" \
	    "send=`cat krffs/rach2_demo_password`" \
	    "expect=# " "send=ifconfig eth0 | grep -q 'addr:0.0.0.0' && echo UNSET\"\"OK" \
	    "expect=UNSETOK" "send=ifup -a" \
	    "expect=lease of 192.168.90.20 obtained" "expect=net: eth0: IPv4 192.168.90.20/" \
	    "send=ifconfig eth0 > /tmp/if; grep -q 'addr:192.168.90.20' /tmp/if && grep -q 'Bcast:192.168.90.255' /tmp/if && grep -q 'Mask:255.255.255.0' /tmp/if && grep -q '192.168.90.3' /etc/resolv.conf && ping 192.168.90.2 > /dev/null && echo LEASE\"\"OK" \
	    "expect=LEASEOK" \
	    "send=arping -f -w 5 192.168.90.2 > /tmp/ap 2>&1; echo ARPING\"\"-STATUS:$$?; cat /tmp/ap" \
	    "expect=ARPING-STATUS:0" "expect=Received 1 response" \
	    "send=arp -n | grep -q '(192.168.90.2)' && echo ARP\"\"CACHED" \
	    "expect=ARPCACHED" \
	    "send=ping 10.9.9.9 > /dev/null 2>&1; echo ROUTE\"\"TRIED" \
	    "expect=ROUTETRIED" "send=echo DHCP\"\"OK" \
	    "expect=DHCPOK"
	# The address, the netmask and the broadcast are asserted above, and the route
	# below: a packet for an address on no subnet of this machine's must leave
	# addressed to the gateway's hardware address, which is what a default route
	# means. Nothing answers 10.9.9.9, and nothing needs to.
	$(PCAPCHECK) bcast $(DHCP_PCAP) \
	    --routed-via 192.168.90.2 --routed-dst 10.9.9.9
ifeq ($(NIC),e1000)
dhcp-stack: dhcp-card
	@echo "dhcp-test: the rows above the driver, the disk's two boots, the unattended boot, the server that never answers, os-simple's client and the fixed addresses, are skipped here; tests/run dhcp covers them on the first card"
else
dhcp-stack: dhcp-card
	# And twice on a disk, where /var/run survives a reboot. `ifup` records there
	# which interfaces it has configured, and the second boot must take a lease
	# rather than read the first boot's record and decide it is already configured.
	# Init empties that directory, and this is what says so: two boots of one disk,
	# each of which must reach a lease.
	$(MKDISK) krffs/manifests/os-real.manifest $(DHCP_DISK) 64 48
	$(HARNESS) $(NIC_DEV) --kernel build/rach2/kernel \
	    --append "rach2.fsdisk rach2.ip=none" \
	    --drive "file=$(DHCP_DISK),format=raw,if=ide,index=0,media=disk" \
	    --qemu-arg=-cpu --qemu-arg=max --net "$(DHCP_NET)" \
	    --timeout 240 --steptmo 60 --ok "FIRSTLEASE" \
	    "expect=rach2 login:" "send=root" "expect=Password:" \
	    "send=`cat krffs/rach2_demo_password`" \
	    "expect=# " "send=ifup -a" \
	    "expect=lease of 192.168.90.20 obtained" "expect=net: eth0: IPv4 192.168.90.20/" \
	    "send=ifconfig eth0 | grep -q 'addr:192.168.90.20' && echo FIRST\"\"LEASE" \
	    "expect=FIRSTLEASE" "send=poweroff -f" until=15 --must-exit
	$(HARNESS) $(NIC_DEV) --kernel build/rach2/kernel \
	    --append "rach2.fsdisk rach2.ip=none" \
	    --drive "file=$(DHCP_DISK),format=raw,if=ide,index=0,media=disk" \
	    --qemu-arg=-cpu --qemu-arg=max --net "$(DHCP_NET)" \
	    --timeout 240 --steptmo 60 --ok "SECONDLEASE" \
	    "expect=rach2 login:" "send=root" "expect=Password:" \
	    "send=`cat krffs/rach2_demo_password`" \
	    "expect=# " "send=ifup -a" \
	    "expect=lease of 192.168.90.20 obtained" "expect=net: eth0: IPv4 192.168.90.20/" \
	    "send=ifconfig eth0 | grep -q 'addr:192.168.90.20' && echo SECOND\"\"LEASE" \
	    "expect=SECONDLEASE" "send=poweroff -f" until=15 --must-exit
	# And unattended, which is how the sealed machine comes up: nothing is typed at
	# all. The metal init runs `ifup -a` itself, after the console login and before
	# the SSH server, so the lease must arrive with no console session in it.
	@rm -f $(METAL_PCAP)
	$(HARNESS) $(NIC_DEV) --kernel build/rach2/kernel \
	    --initrd $(METAL_IMAGE) --qemu-arg=-cpu --qemu-arg=max \
	    --net "$(DHCP_NET),hostfwd=tcp::$(DHCP_SSH_PORT)-:22" \
	    --qemu-arg=-object \
	    --qemu-arg=filter-dump,id=mt0,netdev=net0,file=$(METAL_PCAP) \
	    --append "rach2.ip=none" \
	    --timeout 180 --steptmo 60 --ok "rach2 login:" \
	    "expect=net: eth0: IPv4 192.168.90.20/" \
	    "tcp=$(DHCP_SSH_PORT)"
	# Since 2026-09-14 what init starts writes to `/var/log/services` and not to
	# the console, so the client's lease line is not on this serial at all and the
	# console can only say the machine got as far as a login. Nothing is typed
	# here, so the wire is the whole proof: the step above knocks on the SSH port
	# through the address the lease gave, and the capture must then hold packets
	# sent FROM that address, which only a machine holding it can send. The knock
	# alone proves nothing, because QEMU's user network accepts the host side of a
	# connection before it knows whether the guest is there. The asking is on the
	# wire too, as a broadcast to port 67, and is required here: without it a
	# machine given that address by hand would satisfy the rest of this row.
	$(PCAPCHECK) bcast $(METAL_PCAP) --port 67 --source 192.168.90.20
	# And on a network with no server at all, which is a cable plugged in later or
	# a switch that has not started forwarding. The client must go to the
	# background and keep asking rather than give up, because a machine that gave
	# up is one nobody can reach until somebody walks to it, and walking to it is
	# what this milestone exists to avoid. The console login must still be there,
	# so a network that never comes up does not take the local way in with it. And
	# the same on the machine that carries none of busybox: os-simple's own `netup`
	# reads the same `/etc/network/interfaces` and its own `dhcpc` takes the lease.
	# Both images end up on the same network by the same exchange, which is the
	# point of having two clients over one kernel interface.
	#
	# This one runs on a /16, and that is what makes the netmask provable: the
	# classful mask for a 10.x address is /8, so a client that took the address and
	# not the mask would reach 10.200.0.1 by ARPing for it directly. The check
	# below requires that packet to have gone to the gateway instead, which only a
	# machine holding the leased /16 does.
	@rm -f $(SIMPLE_PCAP)
	$(HARNESS) $(NIC_DEV) --kernel build/rach2/kernel \
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
	    "expect=lease of 10.90.0.20 netmask 255.255.0.0" \
	    "send=ping 10.90.0.2" "expect=4/4 replies" \
	    "send=cat /etc/resolv.conf" "expect=nameserver 10.90.0.3" \
	    "send=netup -a" "expect=is already holding it" \
	    "send=ping 10.200.0.1" "expect=0/4 replies" \
	    "send=dhcpc -i eth0 -k" "expect=net: eth0: IPv4 none" "expect=stopped process" \
	    "send=ping 10.90.0.2" "expect=0/4 replies" \
	    "send=netup -a" "expect=lease of 10.90.0.20 netmask 255.255.0.0" \
	    "send=ping 10.90.0.2" "expect=4/4 replies"
	# The last four steps are the way back off a lease. A client holds an address
	# for as long as it renews it, so an address is taken off by stopping its
	# client: `dhcpc -k` signals the one that holds the interface and waits for the
	# claim to go. The ping that follows must fail, which is what says the address
	# really came off and not just the process; the `netup -a` after it must take a
	# lease again, which is what says the claim was released; and the ping after
	# that must answer, which is what says the machine is back where it started.
	#
	# Every `expect` above is an assertion, and a missed one fails the run. The
	# success marker is a line only the client prints, because this image runs the
	# ISH shell, which does not take the quotes out of an echoed token the way ash
	# does: a token typed at it would match its own echo.
	$(PCAPCHECK) bcast $(SIMPLE_PCAP) --source 10.90.0.20 \
	    --routed-via 10.90.0.2 --routed-dst 10.200.0.1

	# And the other half of the file: a fixed address, on both machines. The format
	# has two methods and only one of them has been exercised above. This is also
	# the sequence an administrator runs to move a machine off DHCP, and every step
	# of it is asserted: the configurator must refuse while a client holds the
	# interface, the client must let go when it is stopped, and the fixed address
	# must then be applied and work. os-simple copies the stanza it stages, since
	# it has no editor; os-real writes one with its shell.
	@rm -f $(STATIC_PCAP)
	$(HARNESS) --kernel build/rach2/kernel \
	    --initrd $(OS_SIMPLE_IMAGE) --qemu-arg=-cpu --qemu-arg=max \
	    --net "$(SIMPLE_NET)" \
	    --qemu-arg=-object \
	    --qemu-arg=filter-dump,id=st0,netdev=net0,file=$(STATIC_PCAP) \
	    --append "rach2.ip=none" \
	    --timeout 240 --steptmo 60 --ok "4/4 replies" \
	    "expect=rach2 login:" "send=root" "expect=Password:" \
	    "send=`cat krffs/rach2_demo_password`" \
	    "expect=The ISH shell" "send=netup -a" \
	    "expect=lease of 10.90.0.20 netmask 255.255.0.0" \
	    "send=cp /etc/network/interfaces.static /etc/network/interfaces" \
	    "send=netup -a" "expect=is holding it with a lease" \
	    "send=dhcpc -i eth0 -k" "expect=net: eth0: IPv4 none" "expect=stopped process" \
	    "send=netup -a" "expect=configured from /etc/network/interfaces" \
	    "send=ping 10.90.0.2" "expect=4/4 replies" \
	    "send=ping 10.200.0.1" "expect=0/4 replies"
	# The address is the fixed one and not the leased one, which the capture says:
	# packets sent FROM 10.90.0.50, and the off-subnet packet addressed to the
	# gateway's hardware address, which is the route the stanza asked for. Neither
	# is possible from a machine still holding the lease.
	$(PCAPCHECK) bcast $(STATIC_PCAP) --source 10.90.0.50 \
	    --routed-via 10.90.0.2 --routed-dst 10.200.0.1
	# The same stanza on os-real, through busybox's `ifup`, which is the whole
	# reason the file has this format: one description of a network, two machines
	# that read it. On the same /16 as the run above, and for the same reason: on a
	# 192.168.x network the mask the stanza asks for is the one the kernel would
	# derive anyway, so skipping the netmask directive entirely would look
	# identical. Here it does not.
	@rm -f $(RSTATIC_PCAP)
	$(HARNESS) --kernel build/rach2/kernel \
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
	    "send=ifup -a" "expect=net: eth0: IPv4 10.90.0.50/" \
	    "send=ifconfig eth0 > /tmp/if; grep -q 'addr:10.90.0.50' /tmp/if && grep -q 'Mask:255.255.0.0' /tmp/if && ping 10.90.0.2 > /dev/null && echo REALSTATIC\"\"OK" \
	    "expect=REALSTATICOK" "send=ping 10.200.0.1 > /dev/null 2>&1; echo DONE" \
	    "expect=DONE" "send=poweroff -f" until=15 --must-exit
	$(PCAPCHECK) bcast $(RSTATIC_PCAP) --source 10.90.0.50 \
	    --routed-via 10.90.0.2 --routed-dst 10.200.0.1

	# And the half of the client that only time reaches: holding the lease. QEMU
	# hands out a day, so `-l` caps what the client will act on, the renewal falls
	# due ten seconds in, and the run must see the lease applied more than once.
	# Without that cap nothing here could reach the renewal path at all, and a
	# client that renewed wrongly would ship green.
	$(HARNESS) --kernel build/rach2/kernel \
	    --initrd $(OS_SIMPLE_IMAGE) --qemu-arg=-cpu --qemu-arg=max \
	    --net "$(SIMPLE_NET)" --append "rach2.ip=none" \
	    --timeout 180 --steptmo 90 --log $(SIMPLE_LOG) \
	    --ok "lease of 10.90.0.20" \
	    "expect=rach2 login:" "send=root" "expect=Password:" \
	    "send=`cat krffs/rach2_demo_password`" \
	    "expect=The ISH shell" "send=dhcpc -i eth0 -l 20" \
	    "expect=renewed for 20 seconds" "expect=renewed for 20 seconds"
	# A renewal and a fresh lease are different things, and the difference is what
	# this checks: the client must say it renewed, and it must have asked for an
	# address exactly once. A client that let every lease lapse and discovered
	# again would print the second line as often as the first.
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

	# And a boot that was told its address on the command line, on a network with a
	# DHCP server ready to hand out a different one. Init must not run `ifup` there
	# at all: a machine given an address is configured already, and a client would
	# begin by taking that address off and might never put one back. This one is
	# asked at the console rather than read off the wire, because what is being
	# tested is that nothing happened.
# interval: the machine is asked every second until init has started SSH
	$(HARNESS) --kernel build/rach2/kernel \
	    --initrd $(METAL_IMAGE) --qemu-arg=-cpu --qemu-arg=max \
	    --net "$(DHCP_NET)" \
	    --append "rach2.ip=192.168.90.50/24 rach2.gateway=192.168.90.2" \
	    --timeout 180 --steptmo 60 --ok "STATICKEPT" \
	    "expect=rach2 login:" "send=root" "expect=Password:" \
	    "send=`cat krffs/rach2_demo_password`" \
	    "expect=# " \
	    "send=until pidof dropbearmulti > /dev/null; do sleep 1; done; ifconfig eth0 | grep -q 'addr:192.168.90.50' && echo STATIC\"\"KEPT" \
	    "expect=STATICKEPT"
# interval: the service log is read every second until the client gives up
	$(HARNESS) --kernel build/rach2/kernel \
	    --initrd $(METAL_IMAGE) --qemu-arg=-cpu --qemu-arg=max \
	    --net "hubport,id=net0,hubid=1" --append "rach2.ip=none" \
	    --timeout 180 --steptmo 90 --ok "NOLEASEOK" \
	    "expect=rach2 login:" "send=root" "expect=Password:" \
	    "send=`cat krffs/rach2_demo_password`" \
	    "expect=# " "send=echo CONSOLE\"\"UP" \
	    "expect=CONSOLEUP" \
	    "send=until grep -q 'no lease, forking to background' /var/log/services; do sleep 1; done; echo NOLEASE\"\"OK" \
	    "expect=NOLEASEOK"
	# The client's giving up is read from the service log and not from this
	# console: init gives what it starts `/var/log/services`, so that a client
	# retrying for minutes does not write over the login prompt.
	# Two cards on two networks, each with a server of its own: a lease on each
	# interface, asked for by name, and a server answering through both. The
	# second client is `udhcpc -i eth1`, which is how an administrator configures
	# an interface that no boot-time file names. Both cards are e1000s whichever
	# card the rest of this suite runs on, since that is the driver with a record
	# per card; the RTL8139 and the RTL8168 drive one each.
	#
	# The knock on each forwarded port proves nothing on its own, since QEMU's
	# user network accepts the host side before it knows whether the guest is
	# there. What proves it is the capture of each network: packets sent FROM the
	# address that network leased, which only a machine holding that address on
	# that card can send, and on the second network that address is not the
	# machine's own. Until TCP took the address a connection arrived on, a
	# connection to the second card was answered from the first card's address
	# and no capture held a packet from the second's.
	@rm -f $(TWONET_PCAP_A) $(TWONET_PCAP_B)
	$(HARNESS) --kernel build/rach2/kernel \
	    --initrd $(OS_REAL_IMAGE) --qemu-arg=-cpu --qemu-arg=max \
	    --net "$(DHCP_NET),hostfwd=tcp::$(DHCP_TWO_PORT_A)-:22" \
	    --device "e1000,netdev=net0" \
	    --qemu-arg=-netdev \
	    --qemu-arg=user,id=net1,net=10.0.7.0/24,host=10.0.7.2,dhcpstart=10.0.7.20,dns=10.0.7.3,hostfwd=tcp::$(DHCP_TWO_PORT_B)-:22 \
	    --qemu-arg=-device --qemu-arg=e1000,netdev=net1 \
	    --qemu-arg=-object \
	    --qemu-arg=filter-dump,id=tw0,netdev=net0,file=$(TWONET_PCAP_A) \
	    --qemu-arg=-object \
	    --qemu-arg=filter-dump,id=tw1,netdev=net1,file=$(TWONET_PCAP_B) \
	    --append "rach2.ip=none" \
	    --timeout 300 --steptmo 90 --ok "TWONETOK" \
	    --log $(IMAGES_DIR)/twonet$(NIC_TAG).log \
	    "expect=rach2 login:" "send=root" "expect=Password:" \
	    "send=`cat krffs/rach2_demo_password`" \
	    "expect=# " "send=ifup -a" \
	    "expect=lease of 192.168.90.20 obtained" "expect=net: eth0: IPv4 192.168.90.20/" \
	    "send=udhcpc -i eth1 -n -q" \
	    "expect=lease of 10.0.7.20 obtained" "expect=net: eth1: IPv4 10.0.7.20/" \
	    "send=ifconfig eth0 > /tmp/i0; ifconfig eth1 > /tmp/i1; grep -q 'addr:192.168.90.20' /tmp/i0 && grep -q 'addr:10.0.7.20' /tmp/i1 && echo TWO\"\"LEASES" \
	    "expect=TWOLEASES" \
	    "send=dropbearmulti dropbear -R -g -p 22" \
	    "tcp=$(DHCP_TWO_PORT_A)" "tcp=$(DHCP_TWO_PORT_B)" \
	    "send=echo TWONET\"\"OK" "expect=TWONETOK"
	$(PCAPCHECK) bcast $(TWONET_PCAP_A) --source 192.168.90.20
	$(PCAPCHECK) bcast $(TWONET_PCAP_B) --source 10.0.7.20
	# And the server's own record of who reached it. The knock says only that
	# QEMU took the host side; this says the segments crossed the interface, were
	# taken by TCP as addressed to the address that card holds, and reached the
	# program listening. A connection arriving on the second card was dropped at
	# the checksum until it was taken against the address the segment was sent
	# to, and answered from the wrong address until the connection recorded the
	# one it arrived on.
	@sh -c 'l=$(IMAGES_DIR)/twonet$(NIC_TAG).log; \
	    grep -aq "Child connection from 192.168.90.2:" "$$l" || \
	        { echo "dhcp-test: FAIL: nothing reached the server on the first card" >&2; \
	          exit 1; }; \
	    grep -aq "Child connection from 10.0.7.2:" "$$l" || \
	        { echo "dhcp-test: FAIL: nothing reached the server on the second card" >&2; \
	          exit 1; }; \
	    echo "dhcp-test: a lease on each interface and the server answering through both: OK"'
endif
.PHONY: dhcp-card dhcp-stack
# `tests/run netconsole`: the kernel log on the wire. The machine this milestone
# targets has no serial port, so the boot log has to leave over Ethernet or not
# at all. QEMU's filter-dump writes every frame the guest sends to a libpcap
# file; the boot below asks for netconsole on the command line, and the checker
# holds the frames to the header's contract and requires a line printed BEFORE
# the network device came up, which is what proves the pre-transmit buffer was
# carried rather than dropped.
#
# Each expected text carries the `] ` that ends a record's prefix, so what is
# required is a whole record reassembled from the frames, not a fragment of
# one. The two rejected texts are the shell's: a prompt and its banner are
# console output, and netconsole carries the log to whoever is on the segment,
# which a person typing at a prompt did not agree to.
NETCONSOLE_PCAP = $(IMAGES_DIR)/netconsole$(NIC_TAG).pcap
NETCONSOLE_STOP_PCAP = $(IMAGES_DIR)/netconsole-stop$(NIC_TAG).pcap
run-netconsole: rach2 tests krffs test-image $(PCAPCHECK) $(HARNESS)
	@rm -f $(NETCONSOLE_PCAP)
	$(HARNESS) $(NIC_DEV) --kernel build/rach2/kernel \
	    --initrd $(TEST_IMAGE) --qemu-arg=-cpu --qemu-arg=max \
	    --qemu-arg=-object \
	    --qemu-arg=filter-dump,id=nc0,netdev=net0,file=$(NETCONSOLE_PCAP) \
	    --append "rach2.netconsole" \
	    --timeout 90 --ok "Type 'help'" "expect=Type 'help'"
	$(PCAPCHECK) netconsole $(NETCONSOLE_PCAP) --min-frames 4 \
	    --expect "] boot: Initializing system components" \
	    --expect "] net: ready:" \
	    --expect "] netconsole: broadcast" \
	    --reject "Type 'help'" \
	    --reject "> "
# And the other end of a netconsole session: the machine stopping. The card is
# silenced on the way out, so where that happens decides what a remote reader
# is told. The contract is that the goodbye line reaches the wire and nothing
# `power_stop` prints after it does, which only a capture can show: a line that
# was never sent and a line that was sent and missed look the same from inside.
	@rm -f $(NETCONSOLE_STOP_PCAP)
	$(HARNESS) $(NIC_DEV) --kernel build/rach2/kernel \
	    --initrd $(TEST_IMAGE) --qemu-arg=-cpu --qemu-arg=max \
	    --qemu-arg=-object \
	    --qemu-arg=filter-dump,id=nc1,netdev=net0,file=$(NETCONSOLE_STOP_PCAP) \
	    --append "rach2.netconsole" \
	    --timeout 90 --ok "Stopping the system" \
	    "expect=Type 'help'" "send=poweroff" until=15 --must-exit
	$(PCAPCHECK) netconsole $(NETCONSOLE_STOP_PCAP) --min-frames 4 \
	    --expect "Stopping the system. Goodbye!" \
	    --reject "powering off"
# `tests/run e1000`: the network path on the other emulated card, and the probe
# order that decides which card a machine with more than one of them gets.
#
# This card is the one QEMU attaches when nobody asks for one, so it is what a
# person running qemu-system-i386 from a generic command line has. The suites
# re-run here are the ones whose subject is the wire: ICMP, the packet socket,
# the interface ioctls, broadcast, netconsole, the raw socket, DHCP, and the
# SSH capstone, which is the deepest use the TCP path gets. The other suites
# are about disks, accounts, and building, and running them again on a second
# card would cost minutes and show nothing.
# `tests/run e1000`: the card selection itself. The eight network suites that
# run on this card as well are separate registry entries, `NAME-e1000`, so the
# pool schedules them beside their first-card twins and can see what each
# costs. This suite ran them itself with an inner `-j4`, which hid nine jobs
# from the pool that was accounting for one.
run-e1000: rach2 tests krffs test-image $(HARNESS)
# The probe order, which only a machine holding more than one card can show.
# The table tries the RTL8139 first, so a machine with both must bind that one;
# a machine with only the e1000 must bind the e1000; and a machine whose card no
# driver claims must say so and still reach its shell, which is what proves each
# driver reports ABSENT for a device that is not its own rather than starting on
# it. The 82574L is that card: Intel, gigabit, and a different register
# interface, so the e1000 driver must refuse it by device id.
#
# Each boot is asserted on the bound driver's own detail line rather than on the
# summary, because the summary names the card in parentheses and the harness
# matcher takes no parenthesis. Only the bound driver prints a detail line, so
# the line is the binding.
	$(HARNESS) --kernel build/rach2/kernel --initrd $(TEST_IMAGE) \
	    --qemu-arg=-cpu --qemu-arg=max \
	    --qemu-arg=-netdev --qemu-arg=user,id=net1 \
	    --qemu-arg=-device --qemu-arg=e1000,netdev=net1 \
	    --timeout 90 --ok "Type 'help'" \
	    "expect=rtl8139: IRQ" "expect=Type 'help'"
	$(HARNESS) --device e1000,netdev=net0 --kernel build/rach2/kernel \
	    --initrd $(TEST_IMAGE) --qemu-arg=-cpu --qemu-arg=max \
	    --timeout 90 --ok "Type 'help'" \
	    "expect=e1000: 82540EM" "expect=Type 'help'"
	$(HARNESS) --device e1000e,netdev=net0 --kernel build/rach2/kernel \
	    --initrd $(TEST_IMAGE) --qemu-arg=-cpu --qemu-arg=max \
	    --timeout 90 --ok "Type 'help'" \
	    "expect=no supported network card" \
	    "expect=Type 'help'"
# `tests/run reset`: each reset mechanism on its own. The kernel tries three,
# most of the machine first, and the order was chosen for a laptop whose chipset
# register may not act; the risk in that choice is that only the first one is
# ever exercised, and nobody finds out the later ones do not work until the
# machine they were written for refuses to restart. So each is booted with the
# ones before it left out, and each must still restart the machine.
run-reset: rach2 tests krffs test-image $(HARNESS)
	sh tests/system/suites/reset.sh build/rach2/kernel $(TEST_IMAGE)
# `tests/run diskdiag`: a disk read without being mounted and without being
# written. Three boots of one freshly built image: the ordinary kernel mounting
# it, which must change it (otherwise the comparisons below prove nothing); the
# read-only kernel reading it over SSH with nothing typed at the console, where
# every sector hashed on the machine must equal the same offset hashed here; and
# the same kernel with no drive at all.
run-diskdiag: rach2 rach2-metal rach2-metal-image $(HARNESS) $(SHA256TOOL) $(RUNUTIL)
	sh tests/system/suites/diskdiag.sh build/rach2/kernel $(METAL_KERNEL) $(METAL_IMAGE) \
	    $(DISKDIAG_PORT)
# `tests/run subset`: each optional device probe left out, and proved left out
# `tests/run usb`: the USB stack, in five boots. Three of them take the 8042
# away, because a key typed or a mouse moved on a machine that still has a PS/2
# keyboard could have arrived by either road, and a check that passes either
# way says nothing about the stack this milestone built.
# `tests/run mount`: the mount milestone's suite. Phase 2 boots the lag device
# and the partition nodes; later phases grow it. Its disk is the test disk
# with a FAT32 partition added at 49 MiB by mkmbr and mtools.
MOUNT_DISK = build/images/mount-test.disk
run-mount: $(DEMO_CREDS) rach2 tests krffs test-image os-real-image $(MKFIXTURE) $(MKMBR) $(HARNESS)
	sh tests/system/suites/mount.sh build/rach2/kernel $(TEST_IMAGE) $(HARNESS) $(MOUNT_DISK) \
	    $(OS_REAL_IMAGE) "`cat krffs/rach2_demo_password`"

# `tests/run load`: the machine under load for twenty seconds and the
# longest delays it saw, from `/proc/stat`, within their bounds.
LOAD_DISK = $(IMAGES_DIR)/load-test.disk
run-load: rach2 tests krffs test-image $(MKMBR) $(HARNESS)
	sh tests/system/suites/load.sh build/rach2/kernel $(TEST_IMAGE) $(HARNESS) $(LOAD_DISK)

# `tests/run conload`: every line typed at the console under load arrives,
# numbered and checksummed, with the reader parked, as type-ahead, after an
# interrupt character to a foreground child, and across an exec.
CONLOAD_DISK = $(IMAGES_DIR)/conload-test.disk
run-conload: rach2 tests krffs test-image $(MKMBR) $(HARNESS)
	sh tests/system/suites/conload.sh build/rach2/kernel $(TEST_IMAGE) $(HARNESS) $(CONLOAD_DISK)

# `tests/run fatwrite`: FAT32 written from a program, read back after a
# reboot, judged by the host's walk and mtools, taken through a crash and its
# repair, written from BusyBox, and written by two processes under the lag.
FATWRITE_DISK = $(IMAGES_DIR)/fatwrite-test.disk
run-fatwrite: $(DEMO_CREDS) rach2 tests krffs test-image os-real-image $(MKFATFIXTURE) $(MKMBR) $(HARNESS)
	sh tests/system/suites/fatwrite.sh build/rach2/kernel $(TEST_IMAGE) $(HARNESS) $(FATWRITE_DISK) \
	    $(OS_REAL_IMAGE) "`cat krffs/rach2_demo_password`" $(MKFATFIXTURE)

# `tests/run loopdev`: a file system in a file, through a loop device: a FAT32
# image and an ext2 image on a FAT32 volume, each mounted, written and read
# back, and everything an attachment refuses.
LOOPDEV_DISK = $(IMAGES_DIR)/loopdev-test.disk
run-loopdev: rach2 tests krffs test-image $(HARNESS) $(MKMBR)
	sh tests/system/suites/loopdev.sh build/rach2/kernel $(TEST_IMAGE) $(HARNESS) \
	    $(LOOPDEV_DISK)

# `tests/run krffsmount`: a KRFFS volume placed by mount(2), read through to
# its last byte and unmounted, and the three volumes the kernel refuses.
KRFFSMOUNT_DISK = $(IMAGES_DIR)/krffsmount-test.disk
run-krffsmount: rach2 tests krffs test-image $(HARNESS) $(MKMBR)
	sh tests/system/suites/krffsmount.sh build/rach2/kernel $(TEST_IMAGE) $(HARNESS) \
	    $(KRFFSMOUNT_DISK) build/krffs/fsck.krffs

# `tests/run ext2`: the ext2 driver booted over a fixture disk the host's
# mke2fs wrote, with the walker's link rows on the mounted volume.
EXT2_DISK = $(IMAGES_DIR)/ext2-test.disk
run-ext2: $(DEMO_CREDS) rach2 tests krffs test-image $(HARNESS) $(MKMBR) \
          utils check-busybox check-tcc check-pdpmake check-setid check-dropbearmulti ports/doom \
          rach2-recovery $(RECOVERY_IMAGE)
	sh tests/system/suites/ext2.sh build/rach2/kernel $(TEST_IMAGE) $(HARNESS) $(EXT2_DISK) \
	    build/tools/image/mkmbr $(RECOVERY_KERNEL) $(RECOVERY_IMAGE)

# `tests/run exfat`: exFAT written from a program, read back after a reboot,
# judged by the host's reader and the external checker where the host has
# one, taken through a crash and its repair, written from BusyBox, and
# written by two processes under the lag.
EXFAT_DISK = $(IMAGES_DIR)/exfat-test.disk
run-exfat: $(DEMO_CREDS) rach2 tests krffs test-image os-real-image $(MKEXFATFIXTURE) $(MKMBR) $(HARNESS)
	sh tests/system/suites/exfat.sh build/rach2/kernel $(TEST_IMAGE) $(HARNESS) $(EXFAT_DISK) \
	    $(OS_REAL_IMAGE) "`cat krffs/rach2_demo_password`" $(MKEXFATFIXTURE)

# `tests/run diskrate`: the storage paths timed in the machine and held under
# bounds: the raw device, a file on exFAT and on FAT32 written and read, two
# hundred small files, and a raw read piped into a hash.
DISKRATE_DIR = $(IMAGES_DIR)/diskrate
run-diskrate: $(DEMO_CREDS) rach2 tests krffs os-real-image test-image $(MKEXFATFIXTURE) $(MKMBR) $(HARNESS) rach2-recovery $(RECOVERY_IMAGE)
	sh tests/system/suites/diskrate.sh build/rach2/kernel $(OS_REAL_IMAGE) $(HARNESS) $(DISKRATE_DIR) \
	    "`cat krffs/rach2_demo_password`" $(MKEXFATFIXTURE) $(MKMBR) $(RECOVERY_KERNEL) $(RECOVERY_IMAGE) \
	    $(TEST_IMAGE)

# The GUID table writer (tools/image/mkgpt.c), for `tests/run usbdisk`: the
# tree had no writer and the suite needs a table to read.
$(MKGPT): tools/image/mkgpt.c
	@mkdir -p $(dir $@)
	cc -O2 -Wall -Wextra -o $@ $<
# The grow set (tools/inspect/growset.c), for `tests/run grow`: the sectors
# a grow writes, checked against what the grow in QEMU changed.
$(GROWSET): tools/inspect/growset.c utils/growset.c
	@mkdir -p $(dir $@)
	cc -O2 -Wall -Wextra -o $@ $<
# The MBR writer, which krffs/mkdisk.sh builds on demand as well; named here
# so a suite that runs before any disk image was made still has it.
$(MKMBR): tools/image/mkmbr.c
	@mkdir -p $(dir $@)
	cc -O2 -o $@ $<
# The stick image's judge (tools/image/stickcheck.c), linked with the
# kernel's own MBR reader so the rules it applies are the kernel's.
$(STICKCHECK): tools/image/stickcheck.c rach2/sys/block/mbr.c rach2/sys/block/mbr.h
	@mkdir -p $(dir $@)
	cc -O2 -Wall -Wextra -I rach2/sys -I rach2/sys/lib -o $@ tools/image/stickcheck.c rach2/sys/block/mbr.c
# The stick image (tools/image/mkstick.sh): the metal hybrid ISO followed by
# an exFAT volume, judged whole by stickcheck and walked by mkexfatfixture.
STICK_IMAGE = $(IMAGES_DIR)/rach2-stick.img
rach2-stick-image: $(STICK_IMAGE)
$(STICK_IMAGE): build/images/rach2-metal.iso tools/image/mkstick.sh $(MKEXFATFIXTURE) $(MKMBR) $(STICKCHECK)
	sh tools/image/mkstick.sh build/images/rach2-metal.iso $@ $(MKEXFATFIXTURE) $(MKMBR) $(STICKCHECK)
build/images/rach2-metal.iso: $(METAL_KERNEL) $(METAL_IMAGE) tools/image/mkiso.sh
	sh tools/image/mkiso.sh rach2-metal
# The install stick (tools/image/mkinstallstick.sh): the recovery ISO followed
# by an exFAT volume carrying the sealed disk image and the manifest naming its
# length and hash. It is what a person boots to install this system.
#
# Both install suites read it, so `prepare-images` builds it once before the
# pool starts: two suites that each named its rule would otherwise run the same
# recovery ISO build at the same time, in the one directory that build uses,
# and neither would find what it wrote. They still name it, so a suite run on
# its own builds what it needs.
rach2-install-stick: $(INSTALL_STICK)
$(INSTALL_STICK): $(RECOVERY_ISO) $(SEALED_DISK) tools/image/mkinstallstick.sh \
                  $(MKEXFATFIXTURE) $(MKMBR) $(STICKCHECK)
	@sh tools/image/keyname.sh $@
	sh tools/image/mkinstallstick.sh $(RECOVERY_ISO) $(SEALED_DISK) $@ \
	    $(MKEXFATFIXTURE) $(MKMBR) $(STICKCHECK)
$(RECOVERY_ISO): $(RECOVERY_KERNEL) $(RECOVERY_IMAGE) tools/image/mkiso.sh tools/image/keyname.sh
	@sh tools/image/keyname.sh $@
	sh tools/image/mkiso.sh $(RECOVERY_NAME)
# A stick of the same shape carrying a 138 MiB disk instead of the sealed one,
# so the rows that install do not each write a gibibyte and a half. The image
# it carries has the shape the sealed disk has, a boot partition,
# a recovery partition and a 64 MiB ext2 root; a machine written from it boots,
# and that ext2 root is what the installer writes accounts into.
# The ext2 root the installer's own disk carries: the same manifest the sealed
# disk's root is built from, since its `init` is the one that starts the SSH
# server and reads the policy the installer writes. It is 64 MiB, the smallest
# that holds it, because every row that installs reads and hashes the whole
# image and the emulator hashes at a megabyte or two a second. Its 16384-block
# groups fill the first 64 MiB group, as growset requires.
INSTALLER_EXT2 = $(IMAGES_DIR)/installer-root.ext2
$(INSTALLER_EXT2): $(FS_DEMO_IMAGES) $(DEMO_CREDS) krffs/manifests/rach2-metal-disk.manifest \
                   $(MANIFEST_DEPS_rach2_metal_disk) \
                   tools/image/mkext2.sh $(IMAGE_TOOLS) | preflight krffs rach2 utils \
                   check-busybox check-tcc check-pdpmake check-setid check-dropbearmulti \
                   ports/doom
	sh tools/image/mkext2.sh krffs/manifests/rach2-metal-disk.manifest $@ 64 16384
INSTALLER_DISK = $(IMAGES_DIR)/installer-test.disk
$(INSTALLER_DISK): $(DEMO_CREDS) $(INSTALLER_EXT2) $(RECOVERY_IMAGE) tools/image/mkdisk_grub.sh \
                   | rach2 rach2-recovery utils krffs
	@mkdir -p $(IMAGES_DIR)
	RACH2_DISK_RECOVERY_MB=8 \
	RACH2_DISK_EXT2_MB=64 \
	RACH2_DISK_EXT2_IMAGE=$(INSTALLER_EXT2) \
	RACH2_DISK_RECOVERY_KERNEL=$(RECOVERY_KERNEL) \
	RACH2_DISK_RECOVERY_IMAGE=$(RECOVERY_IMAGE) \
	    sh tools/image/mkdisk_grub.sh build/rach2/kernel $@
$(INSTALL_TEST_STICK): $(RECOVERY_ISO) $(INSTALLER_DISK) $(INSTALLER_EXT2) \
                       tools/image/mkinstallstick.sh $(MKEXFATFIXTURE) $(MKMBR) $(STICKCHECK)
	sh tools/image/mkinstallstick.sh $(RECOVERY_ISO) $(INSTALLER_DISK) $@ \
	    $(MKEXFATFIXTURE) $(MKMBR) $(STICKCHECK) $(INSTALLER_EXT2)
# `tests/run installer`: the install stick and the program on it. The stick is
# booted through the firmware with no kernel given to the emulator, so what runs
# is what GRUB reads off it.
INSTALLER_DIR = $(IMAGES_DIR)/installer
INSTALLER_PORT = 2327
run-installer: $(DEMO_CREDS) $(INSTALL_STICK) $(INSTALL_TEST_STICK) $(PTYDRIVE) $(HARNESS) $(GROWSET) $(MKEXFATFIXTURE) $(RUNUTIL)
	sh tests/system/suites/installer.sh $(INSTALL_STICK) $(SEALED_DISK) $(HARNESS) \
	    $(INSTALLER_DIR) "`cat krffs/rach2_demo_password`" $(INSTALL_TEST_STICK) \
	    $(INSTALLER_DISK) $(INSTALLER_PORT) $(PTYDRIVE) krffs/rach2_demo_ed25519 install
# `tests/run installer-repair`: the other half, the machine that will not boot.
# Its own entry so the pool runs the two beside each other: each is minutes of
# hashing in the emulator, and one after the other they are the longest suite
# in the tree.
INSTALLER_REPAIR_DIR = $(IMAGES_DIR)/installer-repair
# The rehearsal of the sitting: the small form, minutes, in the registry;
# RACH2_REHEARSE_LENOVO runs the same rows at the machine's size, most of
# an hour. Its directory is its own, since the suite kills what runs under
# its directory at its start and the installer suite runs beside it in
# the pool.
INSTALLER_REHEARSE_DIR = $(IMAGES_DIR)/installer-rehearse
run-installer-rehearse: $(DEMO_CREDS) $(INSTALL_STICK) $(INSTALL_TEST_STICK) $(PTYDRIVE) $(HARNESS) $(GROWSET) $(RUNUTIL)
	sh tests/system/suites/installer.sh $(INSTALL_STICK) $(SEALED_DISK) $(HARNESS) \
	    $(INSTALLER_REHEARSE_DIR) "`cat krffs/rach2_demo_password`" $(INSTALL_TEST_STICK) \
	    $(INSTALLER_DISK) $(INSTALLER_PORT) $(PTYDRIVE) krffs/rach2_demo_ed25519 rehearse

run-installer-repair: $(DEMO_CREDS) $(INSTALL_STICK) $(INSTALL_TEST_STICK) $(PTYDRIVE) \
                      $(HARNESS) $(RUNUTIL)
	sh tests/system/suites/installer.sh $(INSTALL_STICK) $(SEALED_DISK) $(HARNESS) \
	    $(INSTALLER_REPAIR_DIR) "`cat krffs/rach2_demo_password`" $(INSTALL_TEST_STICK) \
	    $(INSTALLER_DISK) $(INSTALLER_PORT) $(PTYDRIVE) krffs/rach2_demo_ed25519 repair \
	    $(INSTALLER_EXT2)
# `tests/run usbdisk`: a flash drive as a disk. Three sticks built on the
# host, an MBR one, a GUID one and one with no table; the nodes, the mounts,
# a write, the pull from under a mount and the replug, ten plugs and pulls
# of a stick nobody opened, BusyBox mounting by label, and a throughput line.
USBDISK_DIR = $(IMAGES_DIR)/usbdisk
run-usbdisk: $(DEMO_CREDS) rach2 tests krffs test-image os-real-image $(MKEXFATFIXTURE) $(MKGPT) $(MKMBR) $(HARNESS) $(STICK_IMAGE) rach2-recovery $(RECOVERY_IMAGE)
	sh tests/system/suites/usbdisk.sh build/rach2/kernel $(TEST_IMAGE) $(HARNESS) $(USBDISK_DIR) \
	    $(OS_REAL_IMAGE) "`cat krffs/rach2_demo_password`" $(MKEXFATFIXTURE) $(STICK_IMAGE) \
	    $(RECOVERY_KERNEL) $(RECOVERY_IMAGE)

run-usb: rach2 tests krffs test-image $(HARNESS) $(MKMBR) harness-kernel
	sh tests/system/suites/usb.sh $(HARNESS_KERNEL) $(TEST_IMAGE) $(HARNESS)
# (see tests/system/suites/subset.sh).
run-subset: rach2 tests krffs test-image $(HARNESS)
	sh tests/system/suites/subset.sh build/rach2/kernel $(TEST_IMAGE) $(HARNESS)
# `tests/run diskpolicy`: the read-only kernel does not write the disk, shown on
# the bytes rather than on a return value. The same disk is booted with each
# kernel; the ordinary one must change it, which is what makes the other half
# mean anything, and the metal one must refuse a command by name and leave the
# image untouched.
run-diskpolicy: rach2 rach2-metal $(HARNESS)
	sh tests/system/suites/diskpolicy.sh build/rach2/kernel $(METAL_KERNEL)
# `tests/run metal`: the metal image reached over SSH with nobody at its
# console. This is the rehearsal for the Lenovo: the machine is given power and
# an address on its command line, and nothing is typed at it. What that proves
# is everything that has to happen by itself for a machine across the room to be
# usable, so the address configuration, init-metal's supervision of dropbear,
# key authentication, a pty, and a file crossing intact each way are all one
# test. It kills the server once as well, because a supervisor that starts a
# daemon and does not restart it has not been tested.
run-metal: rach2-metal rach2-metal-image $(MKMBR) $(PTYDRIVE) $(RUNUTIL)
	sh tests/system/suites/metal.sh $(METAL_KERNEL) $(METAL_IMAGE) $(METAL_PORT)
# `tests/run account`: accounts created, authenticated, changed and deleted on
# both images, since both have account tools now and they are different code.
# os-real gets busybox's over three boots, so what is asserted is what reached
# the platter and not what one process had in its cache; os-simple gets this
# project's own in RAM, where there is no `deluser` to finish with. Both
# authenticate from an unprivileged account, since root's `su` asks for no
# password and would pass over a wrong hash and an unusable set-id `passwd`
# alike.
ACCOUNT_TEST_DISK = $(IMAGES_DIR)/account-test.disk
run-account: $(DEMO_CREDS) rach2 utils krffs busybox ish check-tcc check-pdpmake os-simple-image $(HARNESS) $(RUNUTIL)
	$(MKDISK) krffs/manifests/os-real.manifest $(ACCOUNT_TEST_DISK) 64 48
	sh tests/system/suites/account.sh build/rach2/kernel $(ACCOUNT_TEST_DISK) \
	    "`cat krffs/rach2_demo_password`" real
	sh tests/system/suites/account.sh build/rach2/kernel $(OS_SIMPLE_IMAGE) \
	    "`cat krffs/rach2_demo_password`" simple
# `tests/run tcc`: the machine compiling a program on itself. Boots os-real from
# a disk as the unprivileged `test` account, writes a C file at the prompt,
# compiles it, runs the result, and then asks for the four kinds of program this
# system cannot run and requires each to be refused with its reason and to leave
# no output behind. The boot carries `rach2.trace=enosys`, so a syscall the
# compiler wants and this kernel lacks is named on the console rather than
# turning into a mysterious failure.
TCC_TEST_DISK = $(IMAGES_DIR)/tcc-test.disk
# `tests/run doom`: the Doom port's zone on a disk root, sized from the
# machine's free memory through a heap that grows with brk; one boot.
DOOM_TEST_DISK = $(IMAGES_DIR)/doom-test.disk
run-doom: $(DEMO_CREDS) rach2 utils krffs check-busybox check-tcc check-pdpmake check-setid check-dropbearmulti ports/doom $(HARNESS)
	$(MKDISK) krffs/manifests/os-real.manifest $(DOOM_TEST_DISK) 64 48
	sh tests/system/suites/doom.sh build/rach2/kernel $(DOOM_TEST_DISK) $(HARNESS) \
	    "`cat krffs/rach2_demo_password`"
run-tcc: $(DEMO_CREDS) rach2 utils krffs check-busybox check-tcc check-pdpmake check-setid check-dropbearmulti ports/doom $(RUNUTIL)
	$(MKDISK) krffs/manifests/os-real.manifest $(TCC_TEST_DISK) 64 48
	sh tests/system/suites/tcc.sh build/rach2/kernel $(TCC_TEST_DISK) \
	    "`cat krffs/rach2_demo_password`" "`cat krffs/rach2_test_password`"
# `tests/run make`: the machine building a program on itself, and the claim a
# compiler alone cannot make: that a second build does nothing, that touching
# one source rebuilds one object, and that the answer survives a reboot. Two
# boots of one disk, since the reboot is half the point.
MAKE_TEST_DISK = $(IMAGES_DIR)/make-test.disk
run-make: $(DEMO_CREDS) rach2 utils krffs check-busybox check-tcc check-pdpmake check-setid check-dropbearmulti ports/doom $(RUNUTIL)
	@mkdir -p $(IMAGES_DIR)
	$(MKDISK) krffs/manifests/os-real.manifest $(MAKE_TEST_DISK) 64 48
	sh tests/system/suites/make.sh build/rach2/kernel $(MAKE_TEST_DISK) \
	    "`cat krffs/rach2_test_password`"
# `tests/run selfbuild`: the machine builds its own kernel. Boots os-real from a
# disk copy as the unprivileged test account, copies /src/rach2 home, and runs
# the staged make and compiler over the staged guest.mk, twice from clean
# workspaces; the two kernels must hash equal. Every wait is bounded, and the
# whole run carries one deadline over the per-step ones.
SELFBUILD_DISK = $(IMAGES_DIR)/selfbuild-test.disk
run-selfbuild: $(DEMO_CREDS) rach2 rach2-recovery rach2-recovery-image utils krffs test-image $(OS_SIMPLE_EXT2) check-busybox check-tcc check-pdpmake check-setid check-dropbearmulti ports/doom $(ELFCHECK) $(RUNTESTS) $(RUNUTIL)
	@mkdir -p $(IMAGES_DIR)
	$(MKDISK) krffs/manifests/os-real.manifest $(SELFBUILD_DISK) 64 48
	sh tests/system/suites/selfbuild.sh build/rach2/kernel $(SELFBUILD_DISK) \
	    "`cat krffs/rach2_test_password`" "`cat krffs/rach2_demo_password`"
# `tests/run selfbuild-ext2`: the self-build on an ext2 root, the os-real
# system at 48 MiB on a partition at LBA 2048 as the KRFFS disk has it, booted
# with rach2.root=ext2; the suite reads the volume back with debugfs and
# e2fsck must find it consistent after the synced stop. One clean kernel
# build and the boot of what it built; the second build and the userland
# half are the KRFFS suite's.
SELFBUILD_EXT2_VOL = $(IMAGES_DIR)/selfbuild-ext2.vol
SELFBUILD_EXT2_DISK = $(IMAGES_DIR)/selfbuild-ext2-test.disk
$(SELFBUILD_EXT2_VOL): $(DEMO_CREDS) krffs/manifests/os-real.manifest $(MANIFEST_DEPS_os_real) tools/image/mkext2.sh $(IMAGE_TOOLS) | preflight krffs rach2 utils check-busybox check-tcc check-pdpmake check-setid check-dropbearmulti ports/doom
	sh tools/image/mkext2.sh krffs/manifests/os-real.manifest $@ 48
run-selfbuild-ext2: $(DEMO_CREDS) rach2 utils krffs test-image check-busybox check-tcc check-pdpmake check-setid check-dropbearmulti ports/doom $(ELFCHECK) $(RUNTESTS) $(SELFBUILD_EXT2_VOL) build/tools/image/mkmbr $(RUNUTIL)
	@mkdir -p $(IMAGES_DIR)
	{ dd if=/dev/zero bs=1048576 count=1 2>/dev/null; cat $(SELFBUILD_EXT2_VOL); \
	  dd if=/dev/zero bs=1048576 count=15 2>/dev/null; } > $(SELFBUILD_EXT2_DISK)
	build/tools/image/mkmbr --lba $(SELFBUILD_EXT2_DISK) 2048:98304:83 > /dev/null
	SELFBUILD_ROOT=ext2 sh tests/system/suites/selfbuild.sh build/rach2/kernel $(SELFBUILD_EXT2_DISK) \
	    "`cat krffs/rach2_test_password`" "`cat krffs/rach2_demo_password`"
# `tests/run ssh`: the SSH capstone with nobody at the keyboard. Boots os-real
# from a disk with the port forwarded, starts dropbear from the console session
# the way the Readme's recipe does, then logs in twice: root by key, and the
# unprivileged account by password, which is the half that reaches /etc/shadow.
# Every wait is bounded, so a hang fails instead of sitting there.
SSH_TEST_DISK = $(IMAGES_DIR)/ssh-test$(NIC_TAG).disk
run-ssh: $(DEMO_CREDS) rach2 utils krffs check-busybox check-tcc check-pdpmake check-setid check-dropbearmulti ports/doom $(PTYDRIVE)
	$(MKDISK) krffs/manifests/os-real.manifest $(SSH_TEST_DISK) 64 48
	sh tests/system/suites/ssh.sh build/rach2/kernel $(SSH_TEST_DISK) \
	    "`cat krffs/rach2_demo_password`" "`cat krffs/rach2_test_password`" \
	    $(SSH_TEST_PORT) $(NIC)
# `tests/run shutdown`: userland stopping the machine, on a disk fixture, with
# every claim of a stop asserted on the QEMU process ending rather than on a
# token, and every image checked by offline fsck afterwards.
SHUTDOWN_TEST_DISK = $(IMAGES_DIR)/shutdown-test.disk
run-shutdown: $(DEMO_CREDS) rach2 utils krffs busybox $(RUNUTIL)
	$(MKDISK) krffs/manifests/shutdown-test.manifest $(SHUTDOWN_TEST_DISK) 64 48
	sh tests/system/suites/shutdown.sh build/rach2/kernel $(SHUTDOWN_TEST_DISK) \
	    build/krffs/fsck.krffs "`cat krffs/rach2_demo_password`"
# `tests/run login`: prove the console cannot fall back to an unauthenticated
# shell. Two images differing only in the init they declare, neither staging
# /bin/login, plus a rescue boot of the real image, which is the way back into
# one that will not offer a prompt.
LOGIN_TEST_STRICT = $(IMAGES_DIR)/login-test-strict.krffs
LOGIN_TEST_SHELL  = $(IMAGES_DIR)/login-test-shell.krffs
run-login: $(DEMO_CREDS) rach2 utils krffs busybox os-real-image $(RUNUTIL)
	$(MKIMAGE) krffs/manifests/login-test-strict.manifest $(LOGIN_TEST_STRICT) 10
	$(MKIMAGE) krffs/manifests/login-test-shell.manifest $(LOGIN_TEST_SHELL) 10
	sh tests/system/suites/login.sh build/rach2/kernel $(LOGIN_TEST_STRICT) \
	    $(LOGIN_TEST_SHELL) $(OS_REAL_IMAGE) root "`cat krffs/rach2_demo_password`"
# `tests/run man`: the manual pages on the machine. Boots os-real, logs in and
# reads rach2(7) through a pipe, which must be the staged page byte for byte,
# and on the console under the pager, whose first and last lines must appear;
# `man`'s exit status proves nothing, since it ignores its pager's.
run-man: $(DEMO_CREDS) rach2 rach2-recovery krffs busybox os-real-image rach2-recovery-image $(HARNESS)
	sh tests/system/suites/man.sh build/rach2/kernel $(OS_REAL_IMAGE) $(HARNESS) \
	    "`cat krffs/rach2_demo_password`" build/man/cat7/rach2.7 $(IMAGES_DIR)/man-test.log \
	    $(RECOVERY_KERNEL) $(RECOVERY_IMAGE)
# `tests/run procfiles`: the machine's own /proc files, on the boots that differ
# and through the programs that read them. Three boots of the test image, one
# per mount source, each of which must name its own; then `ps`, `df`, `free` and
# `uptime` on the real image, each of which failed on a missing /proc file.
PROCFILES_DISK = $(IMAGES_DIR)/procfiles-test.disk
run-procfiles: $(DEMO_CREDS) rach2 tests utils krffs busybox os-real-image test-image $(HOSTPEER) $(RUNUTIL)
	$(MKDISK) krffs/manifests/disk-test.manifest $(PROCFILES_DISK) 64 48
	sh tests/system/suites/procfiles.sh build/rach2/kernel $(TEST_IMAGE) \
	    $(PROCFILES_DISK) $(OS_REAL_IMAGE) root "`cat krffs/rach2_demo_password`" 16 \
	    $(PROCFILES_PORT)
# `tests/run exitline`: the plain process-exit line is printed under the debug
# shell and nowhere else. Four boots: an init image that must stay quiet while a
# forked program runs, the test image that must still print it for a child and a
# probe, a killed probe whose dedicated marker must survive, and a the harness
# run with no token, which has only the plain line to read.
HARNESS_KERNEL = build/rach2-harness/kernel

# The kernel built with the harness exit port, which two suites boot. It is a
# prerequisite of those two and of `prepare-all`, never a step of a recipe: a
# recipe that ran `make` in `rach2/` while the pool was running would be a
# second build of one tree beside the first, and two builds of one tree write
# over each other. As a prerequisite it is built once, before the pool, or by
# the one `make` a suite run alone is.
harness-kernel:
	$(MAKE) -C rach2 COMPONENT=rach2-harness HARNESS=1

run-exitline: $(DEMO_CREDS) rach2 tests utils krffs busybox os-real-image test-image $(HARNESS) harness-kernel $(RUNUTIL)
	sh tests/system/suites/exitline.sh build/rach2/kernel $(TEST_IMAGE) \
	    $(OS_REAL_IMAGE) root "`cat krffs/rach2_demo_password`" \
	    $(HARNESS_KERNEL)
run-crash: rach2 tests krffs $(RUNUTIL)
	$(MKDISK) krffs/manifests/disk-test.manifest $(CRASH_TEST_IMAGE) 64 48
	sh tests/system/suites/crash.sh build/rach2/kernel $(CRASH_TEST_IMAGE) build/krffs/fsck.krffs
# `tests/run stress`: prove an on-disk KRFFS larger than physical RAM works.
# Boots a 64 MB disk with a 48 MB partition and runs stressprobe, which writes
# and re-reads 33 MiB (past the 32 MiB RAM cap) through the 4 MiB cache. The
# run ends on the probe's token; the bound is the run's budget and not its
# cost, since the 33 MiB take about 130 s under either accelerator and a
# bound of 150 s cut a loaded emulator's run short. A hang is the runner's
# idle limit to catch.
STRESS_DISK = $(IMAGES_DIR)/stress-test.disk
run-stress: rach2 tests krffs $(HARNESS)
	$(MKDISK) krffs/manifests/stress-test.manifest $(STRESS_DISK) 64 48
	$(HARNESS) --kernel build/rach2/kernel \
	    --append "rach2.fsdisk rach2.rescue" \
	    --drive "file=$(STRESS_DISK),format=raw,if=ide,index=0,media=disk" \
	    --timeout 900 --steptmo 800 --ok "STRESSPROBE OK" --never "STRESSPROBE FAIL" \
	    --log $(STRESS_DISK).log probe=/stressprobe
# The AHCI variants of the disk tests: identical images and probes, but the disk
# rides an ich9-ahci HBA (if=none + ide-hd) and rach2.disk=ahci selects the AHCI
# backend, exercising the same page-cache/KRFFS path through
# ahci_read/ahci_write. The --qemu-arg passthrough adds the two -device lines
# without disturbing the fixed NIC. The ATA disk, persist and stress suites
# stay on if=ide.
AHCI_ATTACH = --qemu-arg=-device --qemu-arg=ich9-ahci,id=ahci0 \
              --qemu-arg=-device --qemu-arg=ide-hd,drive=drv0,bus=ahci0.0
# Per-bus image names so an ATA target and its AHCI twin can run under `make -j`
# without racing on a shared file (each recipe re-stages its image first).
DISK_TEST_AHCI_IMAGE = $(IMAGES_DIR)/disk-test-ahci.disk
# The q35 run boots the same fixture as the AHCI one but must not share the
# file: each recipe stages its own copy, and two suites staging one path at the
# same time leaves one of them checking a file the other has just renamed away.
DISK_TEST_Q35_IMAGE = $(IMAGES_DIR)/disk-test-q35.disk
PERSIST_AHCI_DISK    = $(IMAGES_DIR)/persist-test-ahci.disk
STRESS_AHCI_DISK     = $(IMAGES_DIR)/stress-test-ahci.disk
run-disk-ahci: rach2 tests krffs $(HARNESS)
	$(MKDISK) krffs/manifests/disk-test.manifest $(DISK_TEST_AHCI_IMAGE) 64 48
	$(HARNESS) --kernel build/rach2/kernel \
	    --append "rach2.disk=ahci rach2.fsdisk rach2.rescue" \
	    --drive "file=$(DISK_TEST_AHCI_IMAGE),format=raw,if=none,id=drv0" \
	    $(AHCI_ATTACH) \
	    --timeout 120 --steptmo 90 --ok "BLOCKPROBE OK" --log $(DISK_TEST_AHCI_IMAGE).log probe=blockprobe
# `tests/run disk-q35`: the same disk proof on the q35 machine, whose AHCI
# controller is built in at 0:31.2, function 2 of the multi-function device 31.
# That is the laptop's exact topology, and no other test exercises it: every
# other AHCI test attaches a separate controller that lands at function 0, so a
# scan that never walked past function 0 would pass all of them and fail on the
# real machine.
run-disk-q35: rach2 tests krffs $(HARNESS)
	$(MKDISK) krffs/manifests/disk-test.manifest $(DISK_TEST_Q35_IMAGE) 64 48
	$(HARNESS) --kernel build/rach2/kernel \
	    --append "rach2.disk=ahci rach2.fsdisk rach2.rescue" \
	    --drive "file=$(DISK_TEST_Q35_IMAGE),format=raw,if=none,id=drv0" \
	    --qemu-arg=-machine --qemu-arg=q35 --qemu-arg=-cpu --qemu-arg=max \
	    --qemu-arg=-device --qemu-arg=ide-hd,drive=drv0,bus=ide.0 \
	    --timeout 120 --steptmo 90 --ok "BLOCKPROBE OK" --log $(DISK_TEST_Q35_IMAGE).log probe=blockprobe
# `tests/run disks`: two disks of different sizes on one AHCI controller,
# both in /proc/disk with their ports and sizes, both with nodes, a
# partition of each mounted and written, and the second disk's file found
# on the second disk by the host.
DISKS_DISK0 = $(IMAGES_DIR)/disks-test-0.disk
DISKS_DISK1 = $(IMAGES_DIR)/disks-test-1.disk
run-disks: rach2 tests krffs test-image $(MKMBR) $(HARNESS)
	sh tests/system/suites/disks.sh build/rach2/kernel $(TEST_IMAGE) $(HARNESS) $(DISKS_DISK0) $(DISKS_DISK1)
# `tests/run parts`: four tables on four AHCI disks (an MBR with logical
# partitions, a GPT, a GPT read from its backup, a blank disk), the nodes
# as the tables number them, the table read again, and the root chosen by
# identity: found when the disks are swapped, refused when two disks have it.
PARTS_DIR = $(IMAGES_DIR)/parts
run-parts: rach2 tests krffs test-image $(MKMBR) $(MKGPT) $(OS_SIMPLE_EXT2) $(HARNESS)
	sh tests/system/suites/parts.sh build/rach2/kernel $(TEST_IMAGE) $(HARNESS) $(MKMBR) $(MKGPT) \
	    $(OS_SIMPLE_EXT2) $(PARTS_DIR)
# `tests/run bus`: four IDE drives on both channels, an AHCI and an IDE disk
# together with the root on each in turn, and a disk past 128 GiB.
BUS_DIR = $(IMAGES_DIR)/bus
run-bus: rach2 tests krffs test-image $(MKMBR) $(HARNESS)
	sh tests/system/suites/bus.sh build/rach2/kernel $(TEST_IMAGE) $(HARNESS) $(MKMBR) $(BUS_DIR)
# `tests/run large`: a file past 4 GiB on ext2 and on exFAT, written, read
# back after a remount, sought, sized, truncated, appended and shared
# between two descriptors, and a write past each format's limit refused by
# name. The exFAT volume comes from the host's own formatter.
LARGE_DIR = $(IMAGES_DIR)/large
run-large: rach2 tests krffs test-image $(MKMBR) $(HARNESS)
	sh tests/system/suites/large.sh build/rach2/kernel $(TEST_IMAGE) $(HARNESS) $(MKMBR) $(LARGE_DIR)
# `tests/run grow`: a 1 GiB ext2 root grown by `growroot` on the recovery
# system to a partition the size of the installed machine's disk, its
# writes checked against the grow set, a grow killed and repaired, one
# interrupted between the entry and the grow, and the memory it needs.
# Both modes also rehearse the machine's save, grow and restore on a small root.
GROW_DIR = $(IMAGES_DIR)/grow
run-grow: $(DEMO_CREDS) rach2 tests krffs test-image $(MKMBR) $(GROWSET) $(HARNESS) \
          utils check-busybox check-dropbearmulti rach2-recovery $(RECOVERY_IMAGE) \
          $(SEALED_DISK) $(INSTALL_TEST_STICK) $(MKEXFATFIXTURE)
	sh tests/system/grow_measure_test.sh
	sh tests/system/machine_grow_check.sh
	sh tests/system/machine_grow_boot.sh
	sh tests/system/machine_grow_space_test.sh
	sh tests/system/suites/grow.sh $(RECOVERY_KERNEL) $(RECOVERY_IMAGE) $(HARNESS) $(MKMBR) $(GROWSET) \
	    $(GROW_DIR)
	sh tests/system/grow_machine.sh $(HARNESS) $(RECOVERY_ISO) $(INSTALLER_DISK) $(SEALED_DISK) $(GROW_DIR)
# `tests/run queue`: the AHCI disks on their interrupts: a throttled disk
# beside a fast one, and the fallback root's faults beside the queue.
QUEUE_DIR = $(IMAGES_DIR)/queue
run-queue: rach2 tests krffs test-image $(MKMBR) $(HARNESS)
	sh tests/system/suites/queue.sh build/rach2/kernel $(TEST_IMAGE) $(HARNESS) $(MKMBR) $(QUEUE_DIR)
run-persist-ahci: rach2 tests krffs $(HARNESS)
	$(MKDISK) krffs/manifests/persist-test.manifest $(PERSIST_AHCI_DISK) 64 48
	$(HARNESS) --kernel build/rach2/kernel \
	    --append "rach2.disk=ahci rach2.fsdisk rach2.rescue" \
	    --drive "file=$(PERSIST_AHCI_DISK),format=raw,if=none,id=drv0" \
	    $(AHCI_ATTACH) \
	    --timeout 60 --ok "PERSISTPROBE OK (wrote)" probe=persistprobe
	$(HARNESS) --kernel build/rach2/kernel \
	    --append "rach2.disk=ahci rach2.fsdisk rach2.rescue" \
	    --drive "file=$(PERSIST_AHCI_DISK),format=raw,if=none,id=drv0" \
	    $(AHCI_ATTACH) \
	    --timeout 60 --ok "PERSISTPROBE OK (read back)" probe=persistprobe
run-stress-ahci: rach2 tests krffs $(HARNESS)
	$(MKDISK) krffs/manifests/stress-test.manifest $(STRESS_AHCI_DISK) 64 48
	$(HARNESS) --kernel build/rach2/kernel \
	    --append "rach2.disk=ahci rach2.fsdisk rach2.rescue" \
	    --drive "file=$(STRESS_AHCI_DISK),format=raw,if=none,id=drv0" \
	    $(AHCI_ATTACH) \
	    --timeout 900 --steptmo 800 --ok "STRESSPROBE OK" --never "STRESSPROBE FAIL" \
	    --log $(STRESS_AHCI_DISK).log probe=/stressprobe
# The style suite (see tools/verify/check.sh): -Werror build, scoped cppcheck,
# shellcheck, the convention grep, clang-format, and a readelf ABI check.
# `tests/run style` names this target.
run-style:
	@./tools/verify/check.sh
# The host suite (see tests/system/suites/host.sh): pure kernel logic compiled
# and run natively under AddressSanitizer + UndefinedBehaviorSanitizer. It waits
# on the vendored musl for the same reason krffs does: mkhash_hosttest names the
# musl crypt in its host-deps, and that source exists only once the tarball is
# extracted. `tests/run host` names this target.
# The e2fsprogs stamp is a dependency because two host tests compile the port's
# own `unix_io.c` out of the extracted tree: without it they build against
# whatever that tree last held, or fail to build at all in a fresh checkout.
run-host: check-libc $(E2FSPROGS_STAMP) $(DEADLINE)
	@sh tests/system/machine_measure_test.sh
	@./tests/system/suites/host.sh

# The runner suite (see tests/system/selftest.sh): tests/run driven against a
# registry and a makefile of its own, in a temporary directory. It boots no
# machine and reads nothing from the product, so it needs no prerequisites.
run-runner: $(HARNESS) $(PTYDRIVE) $(HOSTPEER)
	@./tests/system/selftest.sh

# The drivers' suite (see tests/system/drivetest.sh): drive.sh and the
# harness's waits against fake machines that choose when they are ready. It
# needs the harness and nothing that boots.
run-drive: $(HARNESS)
	@./tests/system/drivetest.sh

# `tests/run imagerace`: two builds of one image at the same moment, which a
# parallel run of the suites produces and which used to take both of them
# down. No machine is booted; the image is built twice and read.
# `tests/run krffscheck`: what the offline checker says about images broken on
# purpose. No machine is booted.
KRFFSCHECK_DIR = $(IMAGES_DIR)/krffscheck
run-krffscheck: krffs
	sh tests/system/krffscheck.sh build/krffs/mkfs.krffs build/krffs/fsck.krffs $(KRFFSCHECK_DIR)

IMAGERACE_IMAGE = $(IMAGES_DIR)/imagerace-test.krffs
run-imagerace: $(TEST_IMAGE) $(OS_REAL_IMAGE) $(DEMO_CREDS) krffs rach2 tests utils musl-programs $(IMAGE_TOOLS)
	sh tests/system/imagerace.sh krffs/manifests/test.manifest $(IMAGERACE_IMAGE) 10 \
	    $(TEST_IMAGE) $(OS_REAL_IMAGE)

# The eight network suites again on the second card. Each is the first-card
# target with `NIC=e1000`, which picks the tagged files and the second set of
# forwarded ports, so a pair can run at the same time. The rows whose subject
# is above the driver belong to the first card's suite: `netif`, `raw` and
# `dhcp` are split into a `-card` half, run on both cards, and a `-stack`
# half that the second card's run skips with a line saying which suite
# covers it. The probe suite is not repeated at all, since none of its
# probes sends a frame.

run-ping-e1000:
	$(MAKE) --no-print-directory NIC=e1000 run-ping

run-packet-e1000:
	$(MAKE) --no-print-directory NIC=e1000 run-packet

run-netif-e1000:
	$(MAKE) --no-print-directory NIC=e1000 run-netif

run-dgram-e1000:
	$(MAKE) --no-print-directory NIC=e1000 run-dgram

run-netconsole-e1000:
	$(MAKE) --no-print-directory NIC=e1000 run-netconsole

run-raw-e1000:
	$(MAKE) --no-print-directory NIC=e1000 run-raw

run-dhcp-e1000:
	$(MAKE) --no-print-directory NIC=e1000 run-dhcp

run-ssh-e1000:
	$(MAKE) --no-print-directory NIC=e1000 run-ssh


# The other products a `run-` target names, built with `prepare-images` before
# a set's pool starts, so that no two pooled suites build one at once; they
# are named here, after their definitions, since a prerequisite named before
# its variable is defined expands to nothing. `tools/verify/check.sh` holds
# every `run-` target's file prerequisites to `prepare-all`'s closure.
prepare-products: $(MKEXFATFIXTURE) $(MKFATFIXTURE) $(OS_SIMPLE_EXT2) $(OS_REAL_SMALL_EXT2) \
                  $(GRUB_TEST_EXT2) \
                  $(STICK_IMAGE) $(SELFBUILD_EXT2_VOL) $(IMAGE_TOOLS)
