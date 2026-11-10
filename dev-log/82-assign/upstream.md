# Upstream faults of milestone 82

Faults this milestone found in another project's code, one section each, in the format of [dev-log/upstream.md](../upstream.md), which indexes them.

## QEMU's intel-iommu leaves CFIS clear when CFI is set

* Project: QEMU 10.2.1, including Ubuntu's `qemu-system-x86` 1:10.2.1+ds-1ubuntu3.3. The official `qemu-10.2.1.tar.xz` is retained under `build/cache/qemu/`, SHA-256 `a3717477d8e2c84d630bfffbc20f6cd3293eb45aa1e6dac6d0cc27689991c9e1`.
* Fails: CFI has no effect on either status or interrupt delivery. In the official archive, `hw/i386/intel_iommu.c:2707`, `vtd_handle_gcmd_write` has no CFI handler. At line 4293, `vtd_interrupt_remap_msi` copies a compatibility-format message unchanged without checking CFI, so `vtd_mem_ir_write` forwards it to the APIC while interrupt remapping is enabled and compatibility messages should be blocked. The initial status read after setting CFI was 0x45000000, with CFIS clear.
* Found: 2026-11-09 on the testing host under WSL, in milestone 82's VT-d driver. The first boot waited a second for CFIS. On 2026-11-10, task 82-3's source inspection established the missing delivery check before running the compatibility-MSI injection.
* Reproduction: with interrupt remapping enabled, set GCMD.CFI and read GSTS.CFIS; then clear CFI and make EDU send a compatibility-format MSI. The unpatched source forwards that message instead of recording reason 0x25. `machine.assign.compat` supplies a delivered remapped MSI as a positive control before the forged message and requires a fault without delivery.
* Evidence: `rach2/sys/arch/x86/iommu/vtd.c`, `vtd_set_gcmd`; `tests/assign/edu.c`, `irq_fault`; `tests/assign/qemu-vtd-cfi.patch`; the initial diagnostic `vtd: dmar0: command: no answer, status 0x45000000`.
* Rach2: the driver initially shortened the CFI status wait. The local QEMU patch makes CFIS follow CFI and blocks compatibility-format messages with reason 0x25 when CFIS is clear. The assignment suite uses a private patched emulator; the system emulator stays installed.
* Status: unreported.
* Next: finish the patched assignment injection, check current upstream and add a QEMU qtest before sending the patch. No upstream report has been sent.
