# tests/system/: the behavioral test machinery

This directory holds everything that runs Rach2 to test it: the QEMU runners under `runner/`, the suite scripts under `suites/`, the capture checkers under `checkers/`, the fixture builders under `fixtures/`, and the real-machine acceptance procedure under `accept/`. The suites themselves are Make targets in `suites.mk`, which the root `Makefile` includes with one line so no test rule sits among the product rules. The guest probe sources stay in [`tests/`](../Readme.md) and the native sanitizer tests in [`tests/host/`](../host/Readme.md).

## The runner

`tests/run NAME [NAME...]`, `tests/run core` and `tests/run all` are the entry points. The runner is a 223-line shell script beside `tests/registry`, and the registry is three fields: the suite's name, the external commands it needs, and its tier. The make target is the name with `run-` in front, so it is not written down; `tests/run style` checks that every name has a target and every target has a name.

Six mechanisms, and the reason for each:

* Names and tools are checked before anything is built. A run that would refuse its last suite for a missing `mformat` refuses it in the first second instead of an hour later.
* `prepare-all` builds the product and the test tools once, before any suite starts. Twenty suites each running Make on the same tree would build the same object twenty times, or race over it.
* The suites then run in a pool, `RACH2_WORKERS` at a time, default twice the processor count. A suite spends most of its life waiting for an emulated machine rather than computing, so the useful count is above the core count; on a ten-core machine the full run takes 1286s at four workers, 587 at twelve, and stops improving at twenty.
* Each pooled suite's output is held and replayed in registry order at the end. Twenty suites writing to one terminal is not a report.
* Registry order is the schedule order, longest suite first. A pool finishes when its longest member does, so a long suite started last leaves the machine idle around it.
* A shim named `qemu-system-i386` goes first on `PATH` and writes its own pid to the ledger of the suite that is running. A machine still alive when its suite ends is named with the suite that left it. Listing every QEMU on the host would work only while one suite ran at a time.

`style` and `host` run alone, before the pool: `style` rebuilds the kernel with its own flags, and both share a scratch directory.

`tests/run runner` (`selftest.sh`) drives the runner against a registry and a makefile of its own in a temporary directory, and boots nothing. It is the only test of the runner itself.

## QEMU runners

* `runner/harness.c` starts `qemu-system-i386` headless, accepts optional initrd, disk, network, device, and extra QEMU arguments, and executes ordered serial steps. Step forms include `expect=`, `send=`, `sendraw=`, `wait=`, `tcp=`, `udp=`, `exec=`, and `probe=`. `sendraw=` writes its text with no newline after it, and reads `\xNN` and the usual escapes, which is how a guest terminal in raw mode is given one byte. `key=`, `mouse=`, `usb=add:` and `usb=del:` go to QEMU's own command channel on a unix socket beside the serial line, because a key pressed, a mouse moved or a device plugged in comes from the emulated hardware rather than from anything the guest can be asked to do. The socket is opened only for a run whose steps need it, so every other suite's command line is unchanged. Each such command waits for the monitor's prompt before the run goes on: a second command written while the first is still being read is the one that is lost, and the key it was carrying never arrives. It enforces a run deadline and per-step deadlines. With a harness kernel it can decode the guest process status through QEMU's `isa-debug-exit` device; `--ok` instead requires a serial token, and `--never` fails the run if a text ever appears, which is how a probe that must be killed is checked.
* `runner/runtests.c` implements the shared boot of the `default` suite, which is the first entry in the core tier. It runs the ordered probe list and scores each probe from the kernel line that returns its transient init process to the debug shell. Every probe the shared boot can set up runs here; the ones that need typed input, a host peer, a disk, or a deliberate fault have a suite of their own, most of them through the harness, which decodes an exit status the same way. `--selftest` checks the scorer without QEMU.
* `runner/hostpeer.c` is the host end of a connection a guest test needs. `hold` keeps a TCP connection open while the machine reads its own `/proc/net/tcp`; `echo` serves TCP and UDP on a port and sends back what arrives, which is what makes the socket probes prove a round trip rather than QEMU's willingness to accept.
* `runner/ptydrive.c` runs a command on a pseudo-terminal of a known size and answers its password prompts from the environment. The SSH suites need a client that reads a password from a terminal.
* `runner/deadline.c` runs one program with a time limit and kills its whole process group. The host suite gives each native test 60 seconds through it.

## Capture checkers and fixtures

* `checkers/wavcheck.c` reads the recording QEMU's `wav` audio backend writes and says whether the machine played the scale it was asked for: each named frequency the loudest in its turn, in the order named. The notes are not equal parts of the recording, so it sweeps rather than dividing. It is the only test tool that links the maths library.
* `checkers/pcapcheck.c` reads Ethernet pcap files produced by QEMU, one subcommand per question. `bcast` checks a broadcast UDP datagram, a packet source address, an ARP ownership reply, or use of a gateway for an off-subnet destination. `netconsole` validates and reassembles Rach2 netconsole frames, including their broadcast address, boot id, sequence, payload length, checksum, and required log text. `rawwire` checks the hop limits and caller-supplied ICMP checksum emitted by the raw-socket probe.
* `fixtures/mkfixture.c` builds both disk fixtures. `boot` rewrites a small MBR disk into the `/dev/bootpart` fixture and fills its FAT-typed partition with the probe's byte pattern; `wide` creates a sparse 260 GB disk with known sectors at 5 GiB and 258 GiB for 64-bit disk-offset tests.

## Suites

The two verification suites come first. `suites/host.sh` (`tests/run host`) discovers every `tests/host/*_hosttest.c`, compiles it with the host C compiler and AddressSanitizer plus UndefinedBehaviorSanitizer, and runs it; a test lists separately compiled sources in repeatable `host-deps:` comments, and the project `deadline` tool limits each test to 60 seconds. The style suite lives under [`tools/verify/`](../../tools/Readme.md) because it checks the tree, not the running system.

The behavioral suites receive built artifacts and credentials from their Make targets. They use disposable copies or dedicated fixtures, and each script that starts QEMU installs cleanup for it.

* `suites/account.sh` (`tests/run account`) checks account creation, authentication, password changes, and database consistency in both os-real and os-simple. The os-real profile also checks deletion across reboots.
* `suites/tcc.sh` (`tests/run tcc`) compiles and runs programs as the unprivileged account, compares a machine-built `cat` with the staged one, and checks the unsupported TinyCC output forms.
* `suites/make.sh` (`tests/run make`) checks incremental pdpmake behavior, dependency rebuilding, `.PHONY`, compiler overrides, and timestamps across a reboot.
* `suites/selfbuild.sh` (`tests/run selfbuild`) builds the kernel twice in clean guest workspaces, requires equal hashes, builds utilities and ISH, extracts and checks the exact kernel, and boots it through the probe suite. It also boots that kernel through GRUB, on a disk assembled natively from the pinned GRUB artifacts.
* `suites/ssh.sh` (`tests/run ssh`) checks root key login, unprivileged password login, pty and terminal-size propagation, and rejected authentication attempts. The pty driver `runner/ptydrive.c` answers the password prompts on a sized terminal.
* `suites/procfiles.sh` (`tests/run procfiles`) checks `/proc/mounts` on RAM, page-cache, and disk roots, then checks the `/proc` data used by `ps`, `df`, `free`, `uptime`, `dmesg`, `route`, `arp`, and `netstat`.
* `suites/login.sh` (`tests/run login`) compares strict and permissive init behavior when `/bin/login` is absent, checks recovery of a stopped console session, and verifies `rach2.rescue`.
* `suites/exitline.sh` (`tests/run exitline`) requires plain process-exit diagnostics under the debug shell, suppresses them under init, and checks both the scoring markers and the no-token harness path.
* `suites/shutdown.sh` (`tests/run shutdown`) checks forced and signal-driven halt, poweroff, and reboot paths, root permission, QEMU termination where required, and offline KRFFS state.
* `suites/bootline.sh` (`tests/run bootline`) checks rejected disk-root command lines, an unmountable disk root, and the rescue exemptions.
* `suites/crash.sh` (`tests/run crash`) checks the dirty-mount flag after a killed boot, clean halt, poweroff, and attempts to clear the root's overlapping set-id bit.
* `suites/diskpolicy.sh` (`tests/run diskpolicy`) proves that the ordinary kernel changes a mounted fixture while the write fails under the read-only kernel and leaves every byte unchanged.
* `suites/diskdiag.sh` (`tests/run diskdiag`) checks read-only disk inspection over unattended SSH, compares selected sectors with the host image, requires the diagnostic disk copy to remain unchanged, and checks the no-disk case.
* `suites/reset.sh` (`tests/run reset`) runs each of the three restart mechanisms with the earlier mechanisms skipped.
* `suites/metal.sh` (`tests/run metal`) boots the metal image without console input, checks SSH and file transfer, kills Dropbear, and requires init to start a different server process. It also runs `accept/metal_accept.sh` against the QEMU guest.
* `suites/drill.sh` (`tests/run drill`) destroys a root partition, enters the recovery image, reflashes the root over SSH, verifies the written extent, and boots the repaired system.
* `suites/menu.sh` (`tests/run menu`) checks the immutable GRUB stub, the rewritable menu slot, fallback numbering, static-address fallbacks, a truncated menu, and in-place slot writes.
* `suites/grub.sh` (`tests/run grub`) alternates marked GRUB core images between the two tail slots and proves each replacement by the marker printed by the running core.
* `suites/install.sh` (`tests/run install`) checks kernel trial, commit, fallback, repair, and determinate interrupted environment states on a GRUB disk.
* `suites/sealed.sh` (`tests/run sealed`) runs the finished sealed disk through an injected-fault sequence over SSH and a separate control run that must use neither fallback nor recovery.
* `suites/socket.sh` (`tests/run socket`) starts one `hostpeer echo` and runs the UDP and TCP probes against it, which is the only place the guest opens a connection of its own and is answered.
* `suites/subset.sh` (`tests/run subset`) boots the kernel with each optional device switched off, and requires the probe to record that it was skipped rather than merely requiring a shell.
* `suites/usb.sh` (`tests/run usb`) is nine boots over the USB stack: enumeration at both device speeds, a key typed through the emulator's monitor reaching the debug shell and then a program, a mouse moved on both axes, a device plugged in and pulled out after the boot, the empty `/proc/usb` a machine booted with `rach2.nousb` has, a device this kernel cannot drive refused once and left alone while the devices beside it are enumerated, and a mouse unplugged and plugged back in beside three such devices, which is what the notebook has soldered to it. Three of the boots take the 8042 away, because a key typed on a machine that still has a PS/2 keyboard could have come from either keyboard, and a check that passes either way says nothing about the stack this suite is for.
* `suites/mount.sh` (`tests/run mount`) is eight boots on a disk whose second partition is a FAT32 volume written by mtools: the partition nodes and a read sleeping through the lag device, a device that never answers in time, the root beside a lagged partition, a write copied before the device sleeps, `mountprobe` with an awk over the unmount records, the mounted half of `lagprobe`, the boot partition refusing the raw writer while mounted, and BusyBox on os-real mounting, listing, reading and unmounting the volume with the listing compared against `mdir`.

The remaining suites run through `runner/harness.c` and `runner/runtests.c` directly from their Make recipes, with no script of their own: `default` and the disk, network, persistence and stress families, and three that exist to run probes the shared boot cannot. `tests/run fault` runs the three probes that must be killed rather than served, and ends by running a program that prints a token, which a kernel that took a fault with it could not do. `tests/run console` types at the console for the two probes that block until something is typed, and asserts the text they read back, not only the count.

## Real-machine acceptance

`accept/metal_accept.sh HOST [PORT]` is the non-destructive scripted half of real-machine acceptance. It reads identity, network, entropy, disk, and device information over root SSH and compares the disk facts for which milestone 24 recorded a baseline. Physical actions and the final disk write remain manual procedures under `dev-log/24-metal/` and `dev-log/25-sealed/`. Acceptance is not a suite: it is never part of `tests/run all` and remains governed by the hardware rules in `CLAUDE.md`.
