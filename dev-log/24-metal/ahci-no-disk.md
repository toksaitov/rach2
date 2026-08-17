# Why the laptop's AHCI probe found no disk: the blind investigation

The first boot of the disk diagnostic on the Lenovo printed `Disk diagnostic... none (no disk answered)` and not one line from either disk driver, after a pause of two to four seconds. The hardware was away until evening, so the cause was argued from the boot log, our driver, and the pinned Linux v6.16 AHCI driver (`drivers/ata/libahci.c`, `ahci.c`, fetched to scratch; never into the tree). This note records the argument so the evening session spends its boots on answers.

## What the log proves

The kernel on the stick (commit `c43f51f`) prints five different lines on the AHCI path: the success line, a host-reset timeout, a port whose device never became ready, a link whose signature is not a disk, and a refused command. All five are absent. So the host reset either completed or was never reached, and if the port walk ran, no port ever reached DET=3 with an active interface: every failure went through a then-silent path. The silent candidates are: discovery (no controller found, a bad ABAR, a failed mapping), a stuck BIOS/OS handoff, a port that would not stop, the all-ports-dead walk, and an IDENTIFY that never completed.

The pause does not discriminate. The all-ports-dead walk costs one second of DET polling per implemented port (two to four ports on this PCH). A stuck handoff or a hung IDENTIFY costs ten million MMIO reads. And an instant discovery failure falls through to the legacy ATA probe, whose ports are undecoded on a machine in AHCI mode, read as all ones, all ones has the busy bit set, and the bounded watchdog spins ten million port reads. Every one of those lands in the observed two to four seconds.

## The prime suspect: our own HBA reset, unfinished

`GHC.HR` resets every port register that is not hardware-initialized. QEMU hides what that means: its model resets every port's `PxCMD` with spin-up and power-on already set (`hw/ide/ahci.c:1682` at v9.0.0), so the link comes back by itself and all three AHCI suites pass. Real silicon resets `PxCMD` to the chip's defaults, and where staggered spin-up is supported (`CAP.SSS=1`, common on mobile PCHs), `PxCMD.SUD` resets to 0 and the AHCI specification (10.4.2) requires SUD=1 for a port reset to act. The interface power state `ICC` is not commanded active either.

Linux does three things after its own controller reset that our driver does none of, and the third is aimed at this exact device:

* `ahci_restore_initial_config` (libahci.c): writes the saved pre-reset `PxCMD` back to every implemented port, immediately after the reset.
* `ahci_power_up` (libahci.c, run per port before anything expects a link): sets `PxCMD.SUD` when `CAP.SSS`, then writes `ICC` active.
* `ahci_intel_pcs_quirk` (ahci.c): for Intel controllers, ORs the port map into PCI config word 0x92 (PCS), re-enabling port PHYs "if platform firmware failed to enable ports". The PCI table applies it to `8086:9c03`, this laptop's SATA controller, by name.

Milestone 10 skipped `GHC.HR` because, without a COMRESET, the reset left QEMU's port unready. This milestone added the reset and the COMRESET, and QEMU approved, because QEMU's reset defaults are "up". Real hardware's defaults are "down", and the reset needs Linux's follow-through, not removal.

## The competing hypotheses, after the adversarial round

An independent review attacked the ranking and converged on the same weak point found here: the discovery walk. `board_init` reading `0:31.0` proves PCI configuration access to device 31, not that the scan's multi-function walk visits function 2, where the SATA controller lives; a scan that never reaches `0:31.2` fails discovery instantly and silently, and the ATA fallback supplies the pause. On every known PCH the function-0 LPC bridge advertises multi-function, so this should not happen, and "should not" is not evidence.

The remaining two: the BIOS/OS handoff, which our driver performs and fails closed on, while Linux never performs it at all (`BOHC` appears in Linux only as a bit name; no code touches it), so a firmware with a broken handoff semaphore fails only us. And our link test requires `SSTS.IPM` active where Linux's link-online test checks DET alone, so a link that immediately entered a low-power state would be silently skipped by us and accepted by Linux.

## The fixes proposed, none yet applied

1. Diagnostic first: on AHCI failure, dump the discovery walk (every controller considered), `CAP`, `CAP2`, `PI`, `GHC`, `BOHC`, PCI config 0x92, and per implemented port `SSTS`/`SCTL`/`PxCMD`/`TFD`/`SIG` into the serial log, which `/dev/bootlog` hands over SSH. The naming kernel (`7061827`) is committed but was not on the stick that produced the silent log; with it plus the dump, one boot decides everything above.
2. Port power-up in the COMRESET, per Linux `ahci_power_up`: set `SUD` and `POD` before driving DET, write `ICC` active after releasing it. Harmless where staggered spin-up is absent, since `SUD` is then read-only 1.
3. The Intel PCS re-enable after the HBA reset, vendor 8086 only, per `ahci_intel_pcs_quirk`.
4. The handoff: bounded attempt, then warn and proceed rather than fail closed, which is still more careful than Linux, which never asks.
5. Judge a link by DET alone and command `ICC` active, per Linux `ata_phys_link_online`.
6. Convert the remaining iteration-counted waits (handoff, host reset, port stop: ten million raw reads each) to `k_udelay`-measured budgets, now that `k_udelay` is proven on the metal.

## Applied blind, ahead of the evening session

All six were implemented (`be2a0a6`), reviewed against the pinned Linux sources with no findings, and verified in QEMU: the three AHCI suites, the disk diagnostic, the disk policy comparison and the full probe suite all pass, and a no-disk boot names its cause and reports what a direct read of `0:31.2` sees. The failure dump prints `CAP`, `CAP2`, `GHC`, `PI`, `BOHC`, the Intel PCS word and per-port `SSTS`/`SCTL`/`SERR`/`CMD`/`TFD`/`SIG` into the log `/dev/bootlog` hands out, so if the machine still finds no disk, one SSH command carries everything the hypotheses above disagree about.
