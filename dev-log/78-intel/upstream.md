# Upstream faults of milestone 78

Each entry follows [the upstream fault format](../upstream.md).

## QEMU replaces TCO status bits on a write that should clear them

* Project: QEMU 10.0.0 and 11.1.3, `hw/acpi/ich9_tco.c` from the corresponding upstream Git tags. No source archive is pinned by this task.
* Fails: `tco_ioport_writew`, lines 163 to 168 in both versions, assigns `val & TCO1_STS_MASK` or `val & TCO2_STS_MASK` to the status register. These are write-one-to-clear status bits: Linux 6.12's `drivers/watchdog/iTCO_wdt.c`, `iTCO_wdt_probe`, writes 8 to TCO1_STS, then 2 and 4 to TCO2_STS to clear old timeout and boot status. QEMU's assignments set the written bits and clear unrelated ones instead.
* Found: 2026-11-10 on the Mac, milestone 78, while implementing the TCO driver and reading QEMU's register model for task 78-1a.
* Reproduction: with the timer halted and its status initially zero, write 8 to TCO1_STS and read it back; hardware's timeout bit stays clear, whereas this model reads back 8. A standalone QEMU qtest of that sequence has not been run.
* Evidence: [QEMU 11.1.3's register writes](https://github.com/qemu/qemu/blob/v11.1.3/hw/acpi/ich9_tco.c#L163), [QEMU 10.0.0's register writes](https://github.com/qemu/qemu/blob/v10.0.0/hw/acpi/ich9_tco.c#L163), and [Linux 6.12's probe](https://github.com/torvalds/linux/blob/v6.12/drivers/watchdog/iTCO_wdt.c#L548). `tests/host/itco_hosttest.c` models write-one-to-clear status; `machine.watchdog.expire` observes the actual watchdog action independently of the status registers and passed on QEMU 11.1.3.
* Rach2: no workaround in the driver. Its writes follow the hardware's clearing sequence, and its version 2 keepalive and time-left operations do not use these status bits. The register model is in the watchdog implementation commit; this entry accompanies the diagnostic tools.
* Status: unreported.
* Next: reproduce with qtest on current upstream, correct both assignments to clear the masked written bits, and add a register readback regression before reporting it.

## Linux takes a second panel's type from the VBT without checking its range

* Project: Linux 6.12 and 6.18, `drivers/gpu/drm/i915/display/intel_bios.c` from the corresponding upstream Git tags. No source archive is pinned by this task.
* Fails: `vbt_get_panel_type`, which begins at line 630 in 6.12 and 634 in 6.18, refuses a first panel's type above 0xf other than 0xff but returns `lfp_options->panel_type2` for a child with the handle LFP2 unchecked, at line 648 in 6.12 and 652 in 6.18. `get_panel_type` then only warns (`drm_WARN_ON` on a type above 0xf other than 0xff) and selects it, and the panel's parsers index the LFP data pointers' sixteen entries and shift the LFP options' per-panel bit fields by it: a type of 0x10 to 0xfe reads past the tables.
* Found: 2026-11-10 on the Mac, milestone 78, while transcribing the VBT's panel decoding for task 78-10a.
* Reproduction: not run on Linux. It needs a VBT with a child device of handle LFP2 and an LFP options block whose `panel_type2` is, say, 0x30, booted on a machine with two panels, or such a table loaded through i915's `vbt_firmware` parameter, with KASAN to show the read past the block.
* Evidence: [Linux 6.12's vbt_get_panel_type](https://github.com/torvalds/linux/blob/v6.12/drivers/gpu/drm/i915/display/intel_bios.c#L630) and [Linux 6.18's](https://github.com/torvalds/linux/blob/v6.18/drivers/gpu/drm/i915/display/intel_bios.c#L634). `tests/host/intel_bios_hosttest.c`, row `vbt_panel_type`, checks Rach2's refusal of a second panel type of 0x30.
* Rach2: `rach2/sys/drm/i915/intel_bios.c`, `vbt_get_panel_type`, checks the second panel's type as the first's and returns -1 for one out of range, in the commit that adds the decoders.
* Status: unreported.
* Next: check current upstream's `vbt_get_panel_type`, confirm the read past the tables with KASAN and a crafted table, and send the one-line range check as a patch to intel-gfx.
