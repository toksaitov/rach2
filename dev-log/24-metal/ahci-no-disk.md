# Diagnosing the laptop's AHCI no-disk result

The first Lenovo disk-diagnostic boot printed `Disk diagnostic... none (no disk answered)` after a pause of two to four seconds. Neither disk driver printed a line. The hardware was unavailable until evening, so the initial diagnosis used the boot log, the Rach2 driver, and the pinned Linux v6.16 AHCI driver. The Linux files, `drivers/ata/libahci.c` and `ahci.c`, were fetched only to scratch space. This note records the reasoning used to select the evening tests.

## Evidence from the log

The stick kernel at commit `c43f51f` prints five distinct AHCI results: success, a host-reset timeout, a device that never became ready, a non-disk link signature, or a rejected command. None appeared. The host reset therefore either completed or was never reached. If port iteration ran, no port reached DET=3 with an active interface; each failure used a path that was silent at the time. Possible paths were controller discovery with no device, an invalid ABAR, a failed mapping, a stuck BIOS/OS handoff, a port that would not stop, all ports remaining dead, or an IDENTIFY command that never completed.

The pause did not distinguish these cases. Polling dead ports takes one second per implemented port, and this PCH has two to four ports. A stuck handoff or IDENTIFY command performs ten million MMIO reads. An immediate discovery failure falls back to the legacy ATA probe. On a machine in AHCI mode, its undecoded ports return all ones, which sets the busy bit and makes the bounded watchdog perform ten million port reads. Each path fits the observed two-to-four-second pause.

## HBA reset and port power

`GHC.HR` resets every port register without a hardware-defined initial value. The QEMU model masks the consequence by resetting each port's `PxCMD` with spin-up and power-on already enabled, at `hw/ide/ahci.c:1682` in v9.0.0. The link then recovers automatically, and all three AHCI suites pass. Physical hardware resets `PxCMD` to its chip defaults. When staggered spin-up is supported, as indicated by `CAP.SSS=1` and commonly found on mobile PCHs, `PxCMD.SUD` resets to 0. AHCI specification section 10.4.2 requires SUD=1 before a port reset can operate. The driver also did not request the active `ICC` interface power state.

After a controller reset, Linux performs three operations absent from the Rach2 driver. The third applies to this controller:

* `ahci_restore_initial_config` in libahci.c restores the saved pre-reset `PxCMD` value for every implemented port immediately after reset.
* `ahci_power_up` in libahci.c runs before a port is expected to have a link. It sets `PxCMD.SUD` when `CAP.SSS` is present, then sets `ICC` to active.
* `ahci_intel_pcs_quirk` in ahci.c ORs the port map into Intel PCI configuration word 0x92, PCS, re-enabling port PHYs "if platform firmware failed to enable ports". The PCI table assigns it directly to this notebook's `8086:9c03` SATA controller.

Milestone 10 skipped `GHC.HR` because the reset left QEMU's port unready without COMRESET. This milestone added both operations, and QEMU passed because its reset defaults are "up". The physical hardware defaults are "down", so the reset requires the additional Linux operations rather than removal.

## Other hypotheses after review

An independent review challenged the ranking and identified controller discovery as the main alternative. A `board_init` read from `0:31.0` proves PCI configuration access to device 31, but does not prove that multifunction enumeration visits function 2, which contains the SATA controller. Failure to reach `0:31.2` would make discovery fail immediately and silently, after which ATA fallback would supply the pause. Every known PCH marks the function-0 LPC bridge as multifunction, but "should not" is not evidence.

Two other hypotheses remained. Rach2 performs the BIOS/OS handoff and fails closed, while Linux does not perform it; `BOHC` appears only as a bit name in the Linux source. A broken firmware handoff semaphore would therefore fail only on Rach2. The Rach2 link test also requires active `SSTS.IPM`, while the Linux online test checks only DET. Rach2 would silently skip a link that entered a low-power state immediately, although Linux would accept it.

## Proposed changes before the hardware test

1. On AHCI failure, log every controller considered during discovery; `CAP`, `CAP2`, `PI`, `GHC`, `BOHC`, and PCI configuration word 0x92; and each implemented port's `SSTS`, `SCTL`, `PxCMD`, `TFD`, and `SIG`. The serial log becomes available over SSH through `/dev/bootlog`. The diagnostic kernel at `7061827` had been committed but was absent from the stick that produced the silent log. Adding the dump would let one boot distinguish all preceding hypotheses.
2. Follow Linux `ahci_power_up` during COMRESET. Set `SUD` and `POD` before driving DET, then set `ICC` to active after releasing it. This is harmless without staggered spin-up because `SUD` is then read-only 1.
3. After HBA reset, re-enable Intel PCS for vendor 8086 only, following `ahci_intel_pcs_quirk`.
4. Bound the handoff attempt, then warn and continue instead of failing closed. This remains more conservative than Linux, which does not request the handoff.
5. Following Linux `ata_phys_link_online`, judge a link by DET alone and request active `ICC`.
6. Convert the remaining waits for handoff, host reset, and port stop from ten million raw reads to budgets measured with `k_udelay`, now that `k_udelay` has been proved on the hardware.

## Changes applied before the hardware test

Commit `be2a0a6` implemented all six changes. Review against the pinned Linux sources found no defects. The three AHCI suites, disk diagnostic, disk-policy comparison, and complete probe suite passed in QEMU. A no-disk boot identifies its failure and reports the result of a direct `0:31.2` read. The failure dump sends `CAP`, `CAP2`, `GHC`, `PI`, `BOHC`, the Intel PCS word, and each port's `SSTS`, `SCTL`, `SERR`, `CMD`, `TFD`, and `SIG` to the log available through `/dev/bootlog`. If the notebook still found no disk, one SSH command would collect the evidence that distinguishes the hypotheses.

## Prior SSD behavior

The owner then recalled related behavior from the SSD upgrade. After the notebook's HDD was replaced with the Samsung SSD, a warm reboot sometimes left the drive invisible even to the firmware and sent the machine into a PXE loop. A complete power-off restored it. Disabling HIPM and DIPM in the Intel storage driver's Windows registry settings prevented the failure. Linux also has a quirk for "Samsung SSD 870*", although that entry is limited to ATI controllers; this notebook showed the behavior with its own controller.

This history supplied a second credible cause for the silent first boot. Firmware enumeration might have left device-initiated power management enabled, allowing the link to enter a low-power state before the probe. This drive might then fail to wake for COMRESET, leaving every port dead as observed. Port spin-up after the Rach2 reset remained the leading hypothesis, and the changes covered both causes. The history also identified a later risk: a link allowed to enter slumber during operation might fail until a power cycle, defeating unattended remote access.

The reset path therefore gained `SCTL.IPM = 7`, which forbids every transition. This is the host-side equivalent of the registry change and matches Linux's `ATA_LPM_MAX_POWER` policy. The driver applies it in the same write that starts COMRESET, and it remains set afterward. All four disk suites still pass. If a warm reboot produces the PXE loop again, the known recovery is a complete power-off. The machine is known to exhibit this behavior independently of the stick.

## Hardware result

The rebuilt stick booted without a pause and found the disk: `8086:9C03 at 0:31.2`, port 0 active, `Samsung SSD 870 EVO 1TB`, and 1953525168 sectors, exactly matching the baseline. The acceptance run read the first sector through the complete stack and produced hash `652ad40f...`, identical to the Windows measurement taken before Rach2 ran on the notebook. Every partition-table entry matched, and no partition was mounted. The measured hash verified the no-write requirement.

The diagnostic output isolated the effective change. The PCS path printed nothing, so the port-enable bits were already set. The handoff also printed nothing, so the firmware either did not advertise it or responded promptly. Port power-up remained, and it was the only covered cause consistent with the original result: every port dead at DET without another error. The missing pause provided additional evidence. The earlier boot waited on ports that could not become active, while the corrected boot found them immediately.

The first hypothesis was correct. The Rach2 HBA reset placed every port in its reset-default power state, which the QEMU model does not expose. The three Linux writes performed before link detection corrected the failure. The diagnostic part of the phase was complete. Disk behavior across warm reboots remained to be tested, and the next plan depended on the owner's clean-disk replacement.
