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

## Linux's graphics 7 command parser allows the timestamp of the video ring's old base

* Project: Linux 6.12, `drivers/gpu/drm/i915/i915_cmd_parser.c` and `i915_reg.h` from the upstream Git tag. No source archive is pinned by this task.
* Fails: `gen7_render_regs` and `gen7_blt_regs`, at lines 631 and 678 of `i915_cmd_parser.c`, allow `RING_TIMESTAMP(BSD_RING_BASE)` and its upper half, which `BSD_RING_BASE` (0x04000, `i915_reg.h` line 290) puts at 0x4358 and 0x435c. From graphics 6 the video ring is at `GEN6_BSD_RING_BASE`, 0x12000, as `intel_engine_cs.c`'s table of engines gives it at line 139, so on Ivybridge and Haswell the allowlist names 0x4358, which is not the video ring's timestamp, and refuses 0x12358, which is. A user batch may then read or write 0x4358 through the privileged shadow.
* Found: 2026-11-10 in a worker's clone under WSL, milestone 78, while transcribing the parser's tables for task 78-8b.
* Reproduction: not run on Linux. On a Haswell, an unprivileged batch with `MI_STORE_REGISTER_MEM` of 0x12358 is run unprivileged and that of 0x4358 privileged, which `drm.debug` shows as a rejected register for the first only. What register, if any, answers at 0x4358 on graphics 7 is not known.
* Evidence: [Linux 6.12's gen7_render_regs](https://github.com/torvalds/linux/blob/v6.12/drivers/gpu/drm/i915/i915_cmd_parser.c#L631), [its gen7_blt_regs](https://github.com/torvalds/linux/blob/v6.12/drivers/gpu/drm/i915/i915_cmd_parser.c#L678) and [its engine bases](https://github.com/torvalds/linux/blob/v6.12/drivers/gpu/drm/i915/gt/intel_engine_cs.c#L139). `tests/host/i915_parser_hosttest.c`, rows `tables: 0x4358` and `tables: Haswell's own video ring timestamp`, check that Rach2 judges both as Linux does.
* Rach2: nothing; `rach2/sys/drm/i915/i915_cmd_parser.c` keeps Linux's tables, since its parser must judge every batch as Linux's does, in the commit that ports them.
* Status: unreported.
* Next: find what 0x4358 is on graphics 7 from Intel's register reference, check current upstream's tables, and propose `GEN6_BSD_RING_BASE` for graphics 7, keeping 0x4358 only if a user of it is found.
