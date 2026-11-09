# Upstream faults of milestone 82

Faults this milestone found in another project's code, one section each, in the format of [dev-log/upstream.md](../upstream.md), which indexes them.

## QEMU's intel-iommu leaves CFIS clear when CFI is set

* Project: QEMU 10.2.1, Ubuntu's `qemu-system-x86` 1:10.2.1+ds-1ubuntu3.3, its emulated `intel-iommu` with `intremap=on`.
* Fails: setting GCMD.CFI, which lets compatibility-format interrupts pass while remapping is on, leaves GSTS.CFIS clear, where the VT-d specification has the status follow the command. The global status read after the command was 0x45000000: the root table, the queue and the remapping table's pointer set, CFIS clear. The line of `hw/i386/intel_iommu.c` that handles the command was not read.
* Found: 2026-11-09, on the testing host under WSL, in milestone 82's VT-d driver, whose first boot under QEMU waited the full second for CFIS and logged `vtd: dmar0: command: no answer, status 0x45000000`.
* Reproduction: boot the x86-64 kernel of the commit before the workaround with `qemu-system-x86_64 -machine q35 -device intel-iommu,intremap=on` and read the log; or write GCMD with CFI set from any guest and read GSTS.
* Evidence: the log line above; the driver's wait, `rach2/sys/arch/x86/iommu/vtd.c`, `vtd_set_gcmd`.
* Rach2: the driver waits a millisecond for CFIS and goes on without a record (the commit "Wait a millisecond for the CFI status, which QEMU's unit does not report").
* Status: unreported.
* Next: read `vtd_handle_gcmd_write` and its callers in current QEMU to see whether CFI is handled at all, and whether compatibility-format interrupts are blocked when it is clear.
