# i915

`sys/drm/i915/` is the driver of Intel's Haswell graphics with GT2, the HD 4400 of the Lenovo G50-70 (PCI 8086:0a16) among them, after Linux 6.12's i915, whose names, register values and device table it takes. It binds the chip at 00:02.0, masks its interrupts, enables its MSI message and registers it as the DRM device `i915` ([`../drm.h`](../drm.h)); every access it makes to the chip goes through one hardware boundary, so that a host model and a test backend stand in for the chip. It does not yet render or set modes: the screen stays the firmware's framebuffer and the backlight `intelbl.c`'s ([`../../display/Readme.md`](../../display/Readme.md)). [i915(4)](../../../../man/man4/i915.4) is the interface a program sees.

## Use

The driver binds a chip only when the kernel command line has `rach2.i915` ([cmdline](../../../cmdline.md)). `drm_start` calls `i915_probe` through `arch_drm_probe(ARCH_DRM_NATIVE)`, after bochs and before the firmware framebuffer's driver. A bind logs:

    i915: interrupts: messages on vector N
    i915: found: 00:02.0: haswell/ult (device id 0a16), graphics 7.5, display version 7.00, gt2, revision 0b
    i915: registered: i915 1.6.0 for pci:0000:00:02.0 on card 0

Without the option the probe logs `i915: 00:02.0: 0a16 left to the firmware's framebuffer; rach2.i915 binds it` and touches nothing. A wait that reaches its bound logs `i915: NAME: timeout after N us: register 0xR reads 0xV`.

## Interfaces

* [`i915.h`](i915.h) is the contract every file of the directory keeps, in its opening comment, and the driver's device, `struct drm_i915_private`, which embeds its `struct drm_device` and is found from it with `to_i915`. `struct i915_platform` is what Linux's device table says of Haswell GT2 (`hsw_gt2_info`): graphics 7.5, display 7, the four engines, the LLC, RC6 without RC6p, the engine reset and the aliasing PPGTT. `i915_ids` holds Linux's twenty GT2 ids, each with its subplatform, and `i915_id_lookup` finds one. `i915_driver_bind` binds a PCI function whose registers a given `struct i915_hw` reaches: the probe's way after mapping BAR 0, and any other backend's, the host model's among them.
* [`i915_hw.h`](i915_hw.h) is the hardware boundary: `i915_read`, `i915_write`, `i915_posting_read`, `i915_rmw`, `i915_read64_2x32`, `i915_delay_us`, the bounded waits `i915_wait_for` and `i915_wait_until`, and `i915_hw_mmio_init`, which makes the backend over BAR 0's registers.
* [`i915_reg.h`](i915_reg.h) has the registers by Linux's names with Haswell's values, in blocks that each name the Linux file they come from: configuration space, BAR 0's layout, uncore and forcewake, interrupts, engines, reset, fences and swizzling, the GTT's entries, RC6 and frequency, power wells, pipes, planes and cursors, DDI and its PLLs, the panel power sequencer and backlight, AUX and GMBUS.
* [`sources.mk`](sources.mk) lists the driver's files for `sys/arch/x86/sources.mk`, since the driver is PC-only.

## Sources and license

`i915_reg.h`, the device ids and the platform's features are transcribed from Linux 6.12's `drivers/gpu/drm/i915/` (`i915_reg.h`, `gt/intel_gt_regs.h`, `gt/intel_engine_regs.h`, `gt/intel_gtt.h`, `intel_mchbar_regs.h`, `intel_pci_config.h`, `i915_pci.c` and the `display/` register headers) and `include/drm/intel/i915_pciids.h` and `i915_drm.h`, which carry the MIT license's permission notice. The order of the interrupt reset follows `i915_irq.c`'s `ilk_irq_reset`. The rest is Rach2's own.

## Design

### The hardware boundary

The driver reaches its chip only through a `struct i915_hw`: a table of `read32`, `write32` and `delay_us` and the backend's own pointer. On the machine the backend is BAR 0's first 2 MiB, the registers, mapped uncached with `map_mmio_typed`, as Linux maps them; the second 2 MiB, the GTT, is the global GTT's, which maps it write-combining, as Linux does, so that no page of BAR 0 is mapped with two cache types. In a host test the backend is the model of the HD 4400 (`tests/host/i915_model.c`); any other backend, such as one that completes work at once for tests in QEMU, is a table of the same three calls. An access past the backend's size reads all ones and writes nothing, as a chip that has gone reads.

Every wait on the chip has a bound in microseconds and a name. It reads at once, then after waits of 1 us doubling to 1 ms, counted by the backend's `delay_us`: the processor's spin on its reference counter on the machine, and the model's clock in a test, so a test of a stuck bit takes no time. A wait at its bound returns `-ETIMEDOUT`, logs its name and what the register read, and leaves the name in `last_timeout`. A wait spins and never sleeps, so its bound is short and a long wait is the caller's sleep between bounded waits.

### Locks

Everything runs under the big kernel lock, as [`../Readme.md`](../Readme.md) says. Beside it the order, outermost first, is a DRM device's `commit_lock`, then the driver's locks of buffers and contexts, which may sleep, then the hardware's: the register lock of forcewake and of any register the interrupt handler also writes, then the interrupt mask's, both taken with interrupts off and never held across a sleep. The interrupt handler takes only the hardware's locks. No lock of the driver's exists yet; each part that adds one adds it in this order.

### Binding

`i915_probe` walks the ids and binds function 0 of each match whose BAR 0 is set. The bind reads the board's ids, the GMCH control word and BDSM, which give the GTT's size and the stolen memory; turns on memory decoding and bus mastering; and then, as Linux's `ilk_irq_reset` on Haswell, masks every source, clears every enable, the display engine's master enable among them, and clears every identity register twice: the display engine's, the GT's, power management's and the south display's, with the error, panel self refresh and south error registers. Only then does it take a vector, register its handler and enable the MSI message, so no source the firmware left enabled can raise one. The handler counts the messages on the device's vector, which nothing raises while every source is masked; the part that unmasks a source handles it there. The DRM device is registered without `DRIVER_MODESET` and `DRIVER_ATOMIC`, as Linux's i915 is on a device without display, so programs see a render device and the console keeps the firmware's framebuffer. A failed registration gives the vector back and restores the command word. The unplug hook masks every source again and gives back the message and its vector; the release hook frees the record.

## Limits

* A chip is bound only under `rach2.i915`, until the driver answers Mesa's queries and drives the display; without it a program would find a card of `i915` whose own ioctls all fail.
* None of the driver's own ioctls is served: each answers `EINVAL`, as the DRM core does for a number a driver lacks.
* No forcewake, power wells, GTT, stolen memory, rings, interrupts beyond the mask, display or backlight yet.
* Haswell GT1 and GT3 chips, which Linux binds with other device data, are not bound.

## Tests

* `tests/host/i915_pci_hosttest.c` binds the driver to the host model of the Lenovo's HD 4400 and probes a bus that holds the chip: the ids against Linux's, the gate, the register mapping, the order of the interrupt masks and the message, the configuration words, the DRM identity, the refusals, a failed registration, the unplug, and the boundary's bounded waits on the model's clock.
* `tests/host/i915_model.c` is the model: BAR 0 as a register file, the configuration space of `tests/host/i915_lenovo.h`, a clock that moves only when the driver waits, a log of every access, and a table of blocks. Each later part of the driver that needs behaviour of the chip adds a block in a file of its own, which `i915_model.c` includes and lists in its table, so that parts written at once never edit one model file.
