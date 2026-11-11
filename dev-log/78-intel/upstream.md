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

## Linux refuses a timeline fence with the error of the entry before it

* Project: Linux 6.12 and 6.18, `drivers/gpu/drm/i915/gem/i915_gem_execbuffer.c` from the corresponding upstream Git tags, the same at these lines in both. No source archive is pinned by this task.
* Fails: `add_timeline_fence_array` declares `err` once, at line 2807, before its loop over the entries, and sets it only for an entry whose syncobj holds a fence, at line 2875, from `dma_fence_chain_find_seqno`. An entry marked `I915_EXEC_FENCE_SIGNAL` at a point its timeline has not reached, the usual way to signal a new point, leaves `err` at -EINVAL and goes on. A later entry of the same call whose syncobj holds no fence and whose flags are 0 skips line 2866's check and line 2875, and line 2877 then refuses the whole call with the earlier entry's -EINVAL, where the same entry alone is passed over.
* Found: 2026-11-10 in a worker's clone under WSL, milestone 78, while porting the timeline fences of EXECBUFFER2 for task 78-11a.
* Reproduction: not run on Linux. EXECBUFFER2 with `I915_EXEC_USE_EXTENSIONS` and one timeline extension of two entries: a timeline syncobj marked SIGNAL at a point above its newest, then a new syncobj with no fence and flags 0. Linux should answer EINVAL; without the first entry it answers 0.
* Evidence: [Linux 6.12's add_timeline_fence_array](https://github.com/torvalds/linux/blob/v6.12/drivers/gpu/drm/i915/gem/i915_gem_execbuffer.c#L2807) and [Linux 6.18's](https://github.com/torvalds/linux/blob/v6.18/drivers/gpu/drm/i915/gem/i915_gem_execbuffer.c#L2807). `tests/host/i915_exec_hosttest.c`, row `timeline: an empty syncobj with neither flag after a new point`, checks that Rach2 passes the entry over.
* Rach2: `rach2/sys/drm/i915/i915_gem_execbuffer.c`, `eb_timeline_fence`, judges each entry by its own lookup, in the commit that adds the timeline fences. Mesa sets WAIT or SIGNAL on every entry, so no known program sees the difference.
* Status: unreported.
* Next: check current upstream's loop, show the refusal with the two-entry call above on a machine with i915, and propose moving `err` into the loop, initialised to 0.

## Linux's i915_error_state answers a write with 0 when it holds no error

* Project: Linux 6.12, `drivers/gpu/drm/i915/i915_gpu_error.c` from the upstream Git tag. No source archive is pinned by this task; 6.18's copy has not been read.
* Fails: `i915_error_state_write`, at line 2447, returns 0 when the open found no error state (line 2454), where it clears the state and returns the count when one is held. A write of one byte or more that returns 0 has written nothing, so a writer that loops until its bytes are taken, as glibc's stdio and BusyBox's `full_write` do, loops for good. The sysfs card file `error`, `error_state_write` at line 2530 of the same file, returns the count either way.
* Found: 2026-11-10 on the Mac, milestone 78, while porting the error state's debugfs file for task 78-13a.
* Reproduction: not run on Linux. On a machine with i915 and no GPU hang since boot, or after the state was cleared, run `echo > /sys/kernel/debug/dri/0/i915_error_state` as root; the shell should spin in write(2).
* Evidence: [Linux 6.12's i915_error_state_write](https://github.com/torvalds/linux/blob/v6.12/drivers/gpu/drm/i915/i915_gpu_error.c#L2447) and [its sysfs error_state_write](https://github.com/torvalds/linux/blob/v6.12/drivers/gpu/drm/i915/i915_gpu_error.c#L2530).
* Rach2: `rach2/sys/drm/i915/i915_debugfs.c`, `error_state_write`, returns the count whether or not a state is held, as the sysfs file does, in the commit that adds the debugfs files.
* Status: unreported.
* Next: read 6.18's and current upstream's `i915_error_state_write`, show the spinning write on a machine with i915, and propose returning `cnt` when no state is held.

## Debian's Chromium loads libpci.so.3 without asking for its package

* Project: Debian 13's `chromium` 156.0.8078.79-1~deb13u1 and `chromium-common` of the same version, from trixie-security in the snapshot `vendor/gnu/debian/fetch_debs.sh` pins.
* Fails: ANGLE in `/usr/lib/chromium/libGLESv2.so` loads `libpci.so.3` with dlopen for the GPU's PCI identity (its strings name `libpci.so.3` and `/sys/bus/pci/`), but neither package's Depends nor Recommends names `libpci3`. Without the library ANGLE reads no identity, and chrome://gpu gives `GPU0: VENDOR= 0x0000 ..., DEVICE=0x0000`, so Chromium's GPU blocklist and driver workarounds see no vendor; with it, the Debian reference guest, whose image has `pciutils`, gives virtio-vga's 0x1af4:0x1050.
* Found: 2026-11-10 on the testing PC, milestone 78, while starting Chromium's GPU process on Crocus on the fake i915 chip for task 78-12a.
* Reproduction: on Debian 13 without `libpci3`, run Chromium on any GPU and read GPU0 in chrome://gpu; install `libpci3` and read it again.
* Evidence: `tests/graphics/browser/ref/chromium-sandbox/result.json`, Debian's chrome://gpu with the identity; the trixie-security Packages index in `build/cache/debian/amd64/`.
* Rach2: `vendor/gnu/debian/browsers.list` stages `libpci3` beside the browsers, in the commit that stages it.
* Status: unreported.
* Next: check Debian's current `chromium` in unstable for the dependency, and on a clean Debian 13 without `libpci3` confirm the 0x0000 identity before reporting it to Debian's bug tracker.

## VK-GL-CTS calls into the Vulkan loader after unloading it when a resource error ends the run

* Project: VK-GL-CTS vulkan-cts-1.4.7.1, `vk-gl-cts-1.4.7.1` in `vendor/gnu/vk-gl-cts/sources.txt`, SHA-256 `2071468a8509528b030c5fc54dbad7f4645bb95c4ccda198e01c722372793462`, built by the GNU layer and run on Debian 13's Mesa 25.0.7 and Vulkan loader.
* Fails: after a case answers ResourceError, `deqp-vk` ends its session (`framework/common/tcuTestSessionExecutor.cpp`, lines 400 to 405, `m_abortSession`), prints "Test run was ABORTED!", unloads `libvulkan.so.1`, and then, still under `main`, jumps into the loader's old text and dies of SIGSEGV. The abort leaves the group hierarchy without leaving its groups, so robustness2's `cleanupGroup` (`external/vulkancts/modules/vulkan/robustness/vktRobustnessExtsTests.cpp`, lines 4820 to 4828), which destroys the static `SingletonDevice` devices (lines 81 to 212), never runs. Which object makes the late call is not named: the binary is stripped, and the frames are in `deqp-vk` under `__libc_start_call_main`, not in an exit handler.
* Found: 2026-11-11 on the Lenovo, milestone 78, in task 78-11c's trial of hasvk on Rach2's trial root, on `dEQP-VK.robustness.robustness2.bind.notemplate.r32i.unroll.nonvolatile.sampled_image.no_fmt_qual.img.samples_4.2d_array.comp`, whose image creation answers `VK_ERROR_OUT_OF_DEVICE_MEMORY` on Haswell under Linux as under Rach2.
* Reproduction: on the Lenovo with hasvk, run `deqp-vk` with that one case in `--deqp-caselist-file`; it exits 139. Under gdb the fault is at 0x7effff853290 with ASLR off; `LD_DEBUG=files` gives `libvulkan.so.1` the base 0x7effff815000 and size 0x82388, so the address is in the loader, and its last lines are the loader's `calling fini` and `destroying link map`. Not run on Linux; the steps are user space only, Debian's glibc, loader and Mesa, which Rach2 runs unchanged.
* Evidence: `dev-log/78-intel/78-11c/robustness-gdb.txt`, `dev-log/78-intel/78-11c/robustness-lddebug.txt`, and the `vm: fault: killing` lines in `dev-log/78-intel/78-11c/dmesg-after.txt`; [the session's abort](https://github.com/KhronosGroup/VK-GL-CTS/blob/vulkan-cts-1.4.7.1/framework/common/tcuTestSessionExecutor.cpp#L400) and [robustness2's cleanupGroup](https://github.com/KhronosGroup/VK-GL-CTS/blob/vulkan-cts-1.4.7.1/external/vulkancts/modules/vulkan/robustness/vktRobustnessExtsTests.cpp#L4820).
* Rach2: nothing. `tests/graphics/runners/deqp.sh` reads each case's result from the process's output, where the case has answered ResourceError before the crash, as in Linux's reference, and starts the process's remaining cases in a new one, so the crash costs no case and is not counted.
* Status: unreported.
* Next: build `deqp-vk` with symbols and take the backtrace, to name the object that calls into the unloaded loader; check current upstream's session executor and robustness singletons; run the case on Linux on any driver that refuses that image, to show the same exit.
