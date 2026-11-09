# 82 assign: plan

Milestone 82 of the graphics stack, whose branch is `milestone/76-display`. The stack is milestones 76 to 83, planned together in [the approved plan](../76-graphics/plan.md) and [its task list](../76-graphics/tasks.md); this file gathers what concerns milestone 82. The research behind it is in [dev-log/76-graphics/](../76-graphics/research/Readme.md).

## The goal

The goal: QEMU on Rach2 gives a PCI device to a guest with DMA and interrupt isolation; on the Lenovo the guest is Debian with Linux's i915 driving the panel, and Rach2 gets the GPU back when the guest ends.

Measured under Linux on the Lenovo: Debian 13.7 translates DMA there with or without `intel_iommu=on`, in 12 IOMMU groups, the GPU alone in group 0 with its RMRR (0xab800000 to 0xaf9fffff, `direct-relaxable`, exactly its 2 MiB GTT and 64 MiB of stolen memory); `intel_iommu=off` removes the groups. Why Debian's kernel translated DMA here without the option, where its configuration leaves `CONFIG_INTEL_IOMMU_DEFAULT_ON` unset and sets `CONFIG_INTEL_IOMMU_DEFAULT_ON_INTGPU_OFF`, is untraced; 82-1 reads it in the source. QEMU 10.0.13 with `-machine q35 -nodefaults -vga none -device vfio-pci,host=0000:00:02.0,bus=pcie.0,addr=02.0,x-igd-opregion=on`, SeaBIOS and the guest's kernel booted by QEMU gave the GPU to a Debian guest with no usable ROM and no legacy IGD mode; the guest's i915 lit the panel and ran glmark2 under Weston at 1185 (GL) and 1183 (GLES), 0.988 and 0.983 of the host's with translation on; after a clean power-off and after a SIGKILL of QEMU under load the GPU came back by its function-level reset ("ready 1023ms after AF_FLR") and a 0.6 s bind of i915, at full speed, with no reboot and no DMAR fault. This reference needs no task; what it left open, a UEFI guest and the guest's own GRUB, is in 82-4.

| Task | What it builds | Waits on |
|---|---|---|
| 82-1 VT-d | The DMAR table, domains, queued invalidation, each reserved region mapped only for the devices its scope names, the graphics RMRR identity-mapped while Rach2's driver owns the GPU and relaxable when the GPU goes to a VM, as Linux reports it, without making other regions relaxable; interrupt remapping for the IOAPIC, MSI and MSI-X with compatibility-format interrupts refused, fault reports; each unit's capabilities obeyed as the Lenovo's require: tables flushed from the cache after every write (neither unit walks them coherently) and the snoop bit left clear on the graphics unit, which lacks snoop control; first against QEMU's emulated intel-iommu, which is coherent and cannot catch either; why Debian's kernel translated DMA on this Lenovo without the option, read in its source | nothing |
| 82-2 VFIO | Type1 containers, groups, vfio-pci regions and interrupts through eventfds, the function-level reset, the IGD's OpRegion region (the VBT's ports and panel data, which by i915's source the guest's HDMI needs; the guest lit the panel without it, measured); BARs mapped into QEMU as device memory; host interrupt vectors taken and given back with each interrupt setup, from a pool wider than today's 16; the reserved regions reported to QEMU as Linux reports them; one owner per device. The legacy VGA ranges, the ROM and the bridge regions of legacy IGD mode are not needed: QEMU used none of them on q35 | 82-1, 76-2 |
| 82-3 Assignment in QEMU | A nested guest, under TCG and under the testing PC's nested KVM, compared before one is chosen, given an emulated device through an emulated intel-iommu; isolation faults counted | 82-2, 81-6 |
| 82-4 The GPU to a Linux VM | Rach2's Intel driver releases the GPU; QEMU with the q35 options above and SeaBIOS, with a UEFI guest and the guest's own GRUB tried beside them; Debian's i915 lights the panel; on a clean exit and on QEMU's death under load, the function-level reset, then Rach2's full bring-up and mode set, as Linux's i915 does | 82-3, 78-10b; 81-3a, 81-4 and 81-6 (VMX, interrupts and QEMU) |

Done: 82-3's isolation faults; on the Lenovo the guest's desktop on the panel, with glmark2's ratio recorded against translation-on Linux in a separate Debian boot with the same workload and settings (82-4b.2; Linux's own: 0.988 GL, 0.983 GLES), a research measurement, not an allowance; the panel back under Rach2 with rendering, a GL readback on Crocus, after a clean exit and after QEMU is killed under load.

## The owner's decisions

The owner approved the graphics plan, draft 6i, with `tasks.md`, for all eight milestones of the stack on 2026-11-09 at 22:07 UTC+6, with each of the nine decisions as the plan recommends:

* A: the udmabuf module in WSL's own kernel, loaded by root after each start of WSL; the accounts in the `render` group.
* B: Firefox decodes video on the GPU and Chromium on the CPU.
* C: WebGPU on the Lenovo for tests only.
* D: Chromium's GPU sandbox as Linux has it, so 76-20 is not filed (140 tasks, 665 subtasks).
* E: the performance gate, no unexplained loss over 10% against Linux on the same Lenovo with the IOMMU's translation off.
* F: the Lenovo's p3 shrunk to 64 GiB, the data partition made from Debian on p5, rehearsed in QEMU first (76-15c).
* G: every download the plan's Costs section lists is approved, from the official sources; no card is filed and no task waits on one.
* H: the second run's Lenovo facts and Linux references committed (`tests/fixtures/lenovo-gpu/`, `tests/graphics/ref/lenovo/`) with the run's reports.
* I: the 32-bit profile leaves out the disk-image suites.

Standing decisions: one shared branch, `milestone/76-display`; Steam stays out of the build.

## Tasks

Each task below is an issue in the GitHub milestone `82 assign`, and each subtask a sub-issue under it; a wait is a blocker on GitHub. Faults found in other projects' code go in `upstream.md` beside this file, in the format [dev-log/upstream.md](../upstream.md) gives.

The goal (plan.md, 82): QEMU on Rach2 gives a PCI device to a guest with DMA and interrupt isolation; on the Lenovo the guest is Debian with Linux's i915 driving the panel, and Rach2 gets the GPU back when the guest ends.

Five tasks: the plan's four, with 82-4 split into its code (82-4a) and its Lenovo step (82-4b). 82-1's read-only probe of the Lenovo's units is in 81-6d, one trial with 81-6a's walk, so that 82-2's code waits on no step on the Lenovo.

### 82-1 VT-d

The DMAR table, units, domains, queued invalidation, reserved regions for the devices their scopes name, interrupt remapping and fault reports, each unit's capabilities obeyed; first against QEMU's emulated intel-iommu.

* Files: `rach2/sys/arch/x86/iommu/` (new: `Readme.md`, `sources.mk`, `dmar.c` for the table, read through ACPICA's table access so that `acpi.c` is not edited, `vtd.c` for units, domains and invalidation, `intremap.c`, `fault.c`), `rach2/sys/iommu.h` (new: the interface VFIO uses), `rach2/sys/arch/x86_64/sources.mk` (one include line), `rach2/sys/arch/x86/ioapic.c` (the remapped redirection entries) and `rach2/sys/pci.c` and `pci.h` (hooks in `pci_msi_enable`, `pci.c:426`, and `pci_msix_enable`, `pci.c:535`, which writes its table entry from `pci_msi_message` itself and which NVMe (`nvme.c:1672`) and 76-7's virtio use, and a record of each enabled MSI and MSI-X entry, for remapped messages; far from 76-2b's and 81-6a's hunks), `rach2/hardware.md`, `rach2/cmdline.md` (`intel_iommu=off` as Linux's), `tests/host/vtd_hosttest.c` (new), `tests/host/acpi_qemu_tables.h` (a DMAR table from QEMU's intel-iommu).
* Waits on: nothing.
* Labels: none.
* Subtasks:
  * 82-1.1 The DMAR table's units, device scopes, RMRRs and the IOAPIC's and HPET's scopes; check: a host test on QEMU's table and on the Lenovo's (`dev-log/70-wifi-chip/linux/dmesg.txt:190-200`: HAW 39, two units, two RMRRs).
  * 82-1.2 Root and context tables, second-level page tables, domains, queued invalidation; tables flushed from the cache after each write when the unit's walks are not coherent (ecap bit 0, clear on both of the Lenovo's units) and the snoop bit left clear on a unit without snoop control (dmar0, ecap 0xf0101a); check: the host test against a model unit that records each flush.
  * 82-1.3 Translation turned on for a unit only while a device in its scope is given to a VM, so that Rach2's own drivers, and 78's gate, run with translation off (Problems, item 6). Since translation is per unit, every other device in that unit's scope gets a pass-through context entry, or an identity domain where the unit offers no pass-through, set before translation is turned on and kept through assignment and teardown, so that the host's own DMA goes on; each RMRR mapped only for the devices its scope names, the graphics RMRR reported relaxable when the GPU goes to a VM; check: the host test's domain contents for the assigned device and the unit's other devices.
  * 82-1.4 Interrupt remapping with compatibility-format interrupts refused (`GCMD.CFI` clear), as Linux sets it, since a compatibility-format message bypasses the table; while remapping is on, the host's IOAPIC entries, MSIs and MSI-X entries go through remapped entries of their own, through `ioapic.c` and the hooks in `pci_msi_enable` and `pci_msix_enable`; each enabled MSI and MSI-X entry, those written before remapping turns on among them, is rewritten in remapped format when it turns on, and in compatibility format when it turns off or the device releases the entry, whose remapping entry is then freed; check: host test rows for both paths through setup and teardown, and a QEMU boot with remapping on whose NVMe disk (MSI-X), AHCI disk (MSI) and network work under traffic.
  * 82-1.5 Fault reports read, counted and logged as `vtd: fault: ...`; check: a fault forced in the host test's model.
  * 82-1.6 Why Debian's kernel translated DMA on this Lenovo without `intel_iommu=on`, read in Linux's source; check: the reason, with file and line, in the Readme.
* Done: against QEMU's emulated intel-iommu Rach2's units come up, map and fault as Linux's; 81-6d reads the Lenovo's units as Linux reads them.

### 82-2 VFIO

Type1 containers, groups, vfio-pci's regions and interrupts through eventfds, the function-level reset, the IGD OpRegion region, BARs as device memory, a wider pool of host vectors, reserved regions as Linux reports them, one owner per device.

* Files: `rach2/sys/vfio/` (new: `Readme.md`, `container.c`, `group.c`, `pci.c`, `igd.c`), `rach2/sys/arch/x86_64/sources.mk` (one line), `rach2/sys/pci.c`, `pci.h` (the reset, after 81-6a), `rach2/sys/arch/x86/isr.h`, `isr.c`, `rach2/sys/arch/x86_64/isr_handler.s`, `idt.c`, `rach2/sys/arch/i686/isr_handler.s`, `idt.c` (the pool, `isr.h:98-99`), the nodes `vfio/vfio` and `vfio/N` through 76-1's devfs registry, the `vfio` row of 76-1's table of `/dev` directories, and `/sys`'s `iommu_group` links, `/sys/kernel/iommu_groups/N/` and `driver_override` through 76-9a's registration interface, so that 82-2 edits none of `devfs.c`, `synthfs.c`, `procfs.c` and `syscall_fs.c`, `man/man4/vfio.4` (new), `man/man5/sysfs.5`, `man/man7/limits.7`, `tests/host/vfio_hosttest.c` (new), `tests/host/pcimsi_hosttest.c`.
* Waits on: 82-1 (code: domains and remapping); 76-2 (code: pinning and device mappings); 81-1a (code: the kernel eventfd interface); 81-6a (code: `pci.c` after the walk); 76-9a (code: the `/sys` registration interface, Problems item 2); 79-9 (code: the x86-64 entry files, `idt.c` and `isr_handler.s`, after 79-1a, 79-3 and 79-9). `isr.c` is edited beside 76-19's return-to-user hook at far-apart hunks.
* Labels: none.
* Subtasks:
  * 82-2.1 The vector pool widened from 16 (`ISR_MSI_FIRST` 48 to `ISR_MSI_LAST` 63) to what the IDT can give, with 0x80 (79-1a's compat system call gate), the exceptions, the legacy IRQ vectors and the LAPIC's own vectors (timer, error, spurious, IPIs) kept out, vectors taken and given back with each interrupt setup; check: `pcimsi_hosttest.c` rows past 16 and back.
  * 82-2.2 `/dev/vfio/vfio`, groups as `/dev/vfio/N`, `/sys/bus/pci/devices/BDF/iommu_group` (QEMU reads it to find the group), `/sys/kernel/iommu_groups/N/{devices,reserved_regions,type}`, a device given to vfio-pci by `driver_override` and refused while Rach2's driver holds it; check: the classifier's host test and a musl row.
  * 82-2.3 Type1: `VFIO_SET_IOMMU`, `VFIO_IOMMU_MAP_DMA` and `UNMAP_DMA` with pinned pages, the IOVA ranges and reserved regions in `VFIO_IOMMU_GET_INFO`; check: host test against 82-1's model.
  * 82-2.4 vfio-pci: regions at Linux's offsets (index shifted by 40 bits, which fits Rach2's 32-bit page offset for indexes below 16), BARs mapped as device memory through 76-2, configuration space with the reset and the IGD OpRegion region; check: host test rows and QEMU's `vfio-pci` reading them in 82-3.
  * 82-2.5 Interrupts: INTx with masking, MSI and MSI-X into eventfds through remapped entries, torn down on release; check: host test rows.
  * 82-2.6 The function-level reset on release and on the owner's death, then the device's state checked; check: host test rows.
  * 82-2.7 `vfio(4)`, `sysfs(5)`, `limits(7)`; check: `make mancheck`.
* Done: the host tests pass, and QEMU's `vfio-pci` opens a device on Rach2 (proved with a guest in 82-3).

### 82-3 Assignment in QEMU

A nested guest given an emulated device through an emulated intel-iommu, under TCG and under the testing PC's nested KVM, compared before one is chosen; isolation faults counted.

* Files: `tests/assign/` (new: the outer machine's recipe, the guest's scripts), `tests/cases/machine.assign.json` (new), `tests/registry` (one row), `tests/system/suites.mk` (its block), `dev-log/82-assign/82-3.md` (new).
* Waits on: 82-2 (code); 81-6b, 81-6c (code: QEMU in the root and the runner); 81-2, 81-4 (code: SVM and interrupts, for the nested-KVM variant, Problems item 10); 76-8 (code: virtio-gpu's driver on 76-7's MSI-X transport, which 82-3.1's remapped host interrupts need).
* Labels: `linux-host`.
* Subtasks:
  * 82-3.1 The outer machine: q35 with `intel-iommu,intremap=on,caching-mode=on`, a NIC that Rach2 does not drive (`virtio-net-pci` or `pcnet`), QEMU's `edu` test device, and for the host an NVMe disk and `virtio-gpu-pci`, which Rach2 drives on MSI-X (76-7.4); check: Rach2 boots and lists the unit.
  * 82-3.2 The NIC given to a Debian guest of Rach2's QEMU under TCG; check: the guest's driver binds and pings the outer gateway.
  * 82-3.3 The same under Rach2's KVM on the nested-KVM profile, and the two compared by exits and time before one is chosen for the suite; check: the comparison in `82-3.md`.
  * 82-3.4 Isolation faults, with the host's disk and network under traffic throughout: `edu`'s DMA to an address outside its domain blocked and counted; an MSI with a wrong index blocked and counted; a compatibility-format MSI from the guest's device blocked and counted; check: the counts, Rach2's memory unchanged and the host's traffic without error.
  * 82-3.5 The guest killed under traffic: reset, the group back, pins and vectors back to their counts before; then the same device given to a second guest; check: the counts, and the second guest's driver binds. These are the cases `assign:kill` and `assign:reassign`.
  * 82-3.6 From the runs of 82-3.2 to 82-3.5, each timed as it ran, the minutes of 82's checkpoint slice recorded in `82-3.md` and the plan's Checkpoints; no extra run.
* Done: assignment works in QEMU on the chosen profile, and every isolation fault is blocked and counted.

### 82-4a GPU handoff

Rach2's Intel driver releases the GPU to vfio-pci and brings it up again from a reset device, proven by a host test of the whole path on the HD 4400 model; QEMU's command line for the Lenovo, tried in QEMU against 82-3's device. Split from its Lenovo step (82-4b), since 83-5b, 83-6c and 83-7 need the code.

* Files: `rach2/sys/drm/i915/i915_handoff.c` (new, Problems item 11), with one line in `rach2/sys/drm/i915/sources.mk`; `tests/host/i915_handoff_hosttest.c` (new) over 78's model; `tests/assign/` (the Lenovo's scripts, after 82-3).
* Waits on: 82-2 (code); 78-10b (code: the panel's full mode set); 82-3 (code: `tests/assign/` and the device the scripts are tried against).
* Labels: `linux-host`.
* Subtasks:
  * 82-4a.1 The driver's release (engines idle, interrupts off, the device handed to vfio-pci) and its bring-up again from a reset device, with no firmware state to adopt; check: `i915_handoff_hosttest.c` runs release, reset and bring-up on the HD 4400 model to a full mode set of the panel, start to finish, as the `machine` skill's readiness note asks.
  * 82-4a.2 QEMU's command line: `-machine q35,kernel-irqchip=split -nodefaults -vga none -device vfio-pci,host=0000:00:02.0,bus=pcie.0,addr=02.0,x-igd-opregion=on`, SeaBIOS and the guest's kernel; with a UEFI guest and the guest's own GRUB tried beside; check: the runner's scripts in QEMU against 82-3's device.
* Done: the handoff host test runs release, reset and bring-up on the model, and the scripts run in QEMU against 82-3's device.

### 82-4b The GPU to a Linux VM on the Lenovo

QEMU on q35 with SeaBIOS gives the Lenovo's GPU to Debian, whose i915 lights the panel; on a clean exit and on QEMU's death under load, the reset, then Rach2's full bring-up and mode set.

* Files: `dev-log/82-assign/82-4.md` (new).
* Waits on: 82-4a (code); 81-1b, 81-3a, 81-4, 81-6b, 81-6c (code: device slots, VMX, interrupts, QEMU, the runner); 76-15b (code: the trial root's slot); 76-15c, 78-10d, 81-3b, 81-6d (`evidence`: the data partition, the display, VMX and the VT-d units proven on the Lenovo).
* Labels: `lenovo`.
* Subtasks:
  * 82-4b.1 The readiness note: why each new path cannot stop the machine (the reset bounded, the bring-up's waits bounded, SSH kept), 82-4a.1's host test as the run of the same path, and the manager's go; hand back `owner-only`; check: the note and the go on the issue.
  * 82-4b.2 The paired Linux reference: glmark2 under Weston on translation-on Linux, with the guest's workload and settings, in a Debian boot of its own, or the stored 0.988 and 0.983 if their settings match the guest's; check: the numbers in `82-4.md`.
  * 82-4b.3 "Lenovo notice:" ten minutes before, since the guest takes the screen; check: the comment's time.
  * 82-4b.4 Lenovo step, a trial boot of its own, `mixer vol 1` after the boot: the guest's i915 lights the panel and runs glmark2 under Weston, its ratio recorded against 82-4b.2's reference; then a clean exit, then QEMU killed under load, each followed by the reset, Rach2's bring-up, a pipe CRC equal to the same mode's before, and a GL readback on Crocus; check: the ratio, the CRCs, the readbacks and no DMAR fault counted.
* Done: as 82's Done (plan.md, 82), with the ratio taken against a separate Debian boot.
