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
