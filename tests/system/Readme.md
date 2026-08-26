# tests/system/: the behavioral test machinery

This directory holds everything that runs Rach2 to test it: the QEMU runners under `runner/`, one entry script per suite under `suites/`, the capture checkers under `checkers/`, the fixture builders under `fixtures/`, and the real-machine acceptance procedure under `accept/`. `tests/run NAME` (the suite runner beside `tests/registry`) is the entry point; it builds a suite's artifacts through Make and then runs the suite's script or recipe. The guest probe sources stay in [`tests/`](../Readme.md) and the native sanitizer tests in [`tests/host/`](../host/Readme.md).

## QEMU runners

* `runner/harness.c` starts `qemu-system-i386` headless, accepts optional initrd, disk, network, device, and extra QEMU arguments, and executes ordered serial steps. Step forms include `expect=`, `send=`, `wait=`, `tcp=`, `udp=`, `exec=`, and `probe=`. It enforces a run deadline and per-step deadlines. With a harness kernel it can decode the guest process status through QEMU's `isa-debug-exit` device; `--ok` instead requires a serial token.
* `runner/runtests.c` implements the shared boot used by `make test`. It runs the ordered probe list and scores each probe from the kernel line that returns its transient init process to the debug shell. Probes that need input, networking, a deliberate fault, or a nonzero contract are run by separate Make recipes through `the harness`. `--selftest` checks the scorer without QEMU.

## Capture checkers and fixtures

* `checkers/pcapcheck.c` reads Ethernet pcap files produced by QEMU, one subcommand per question. `bcast` checks a broadcast UDP datagram, a packet source address, an ARP ownership reply, or use of a gateway for an off-subnet destination. `netconsole` validates and reassembles Rach2 netconsole frames, including their broadcast address, boot id, sequence, payload length, checksum, and required log text. `rawwire` checks the hop limits and caller-supplied ICMP checksum emitted by the raw-socket probe.
* `fixtures/mkfixture.c` builds both disk fixtures. `boot` rewrites a small MBR disk into the `/dev/bootpart` fixture and fills its FAT-typed partition with the probe's byte pattern; `wide` creates a sparse 260 GB disk with known sectors at 5 GiB and 258 GiB for 64-bit disk-offset tests.

## Suites

The two verification suites come first. `suites/host.sh` (`tests/run host`) discovers every `tests/host/*_hosttest.c`, compiles it with the host C compiler and AddressSanitizer plus UndefinedBehaviorSanitizer, and runs it; a test lists separately compiled sources in repeatable `host-deps:` comments, and the project `deadline` tool limits each test to 60 seconds. The style suite lives under [`tools/verify/`](../../tools/Readme.md) because it checks the tree, not the running system.

The behavioral suites receive built artifacts and credentials from their Make targets. They use disposable copies or dedicated fixtures, and each script that starts QEMU installs cleanup for it.

* `suites/account.sh` (`tests/run account`) checks account creation, authentication, password changes, and database consistency in both os-real and os-simple. The os-real profile also checks deletion across reboots.
* `suites/tcc.sh` (`tests/run tcc`) compiles and runs programs as the unprivileged account, compares a machine-built `cat` with the staged one, and checks the unsupported TinyCC output forms.
* `suites/make.sh` (`tests/run make`) checks incremental pdpmake behavior, dependency rebuilding, `.PHONY`, compiler overrides, and timestamps across a reboot.
* `suites/selfbuild.sh` (`tests/run selfbuild`) builds the kernel twice in clean guest workspaces, requires equal hashes, builds utilities and ISH, extracts and checks the exact kernel, and boots it through the probe suite. It also boots that kernel through GRUB when Docker is available.
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

The remaining suites (`default`, the disk, network, persistence, and stress families) run through `runner/harness.c` and `runner/runtests.c` directly from their Make recipes without a suite script.

## Real-machine acceptance

`accept/metal_accept.sh HOST [PORT]` is the non-destructive scripted half of real-machine acceptance. It reads identity, network, entropy, disk, and device information over root SSH and compares the disk facts for which milestone 24 recorded a baseline. Physical actions and the final disk write remain manual procedures under `dev-log/24-metal/` and `dev-log/25-sealed/`. Acceptance is not a suite: it is never part of `tests/run all` and remains governed by the hardware rules in `CLAUDE.md`.
