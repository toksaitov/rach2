# Notes for the 75-intel breakdown

Drafter's working notes. Output: `75-intel.md` beside this file.

## Read so far

* `breakdown/brief.md`: the form (70-wifi-chip's plan), the sections, the rules.
* `plan.md` whole: 75's table (75-1 to 75-14), its Done, Decisions (plan's 4, 12, 13), Order, Risks 6, 10, What is left out, the research notes on 75 (Muse's points on reset, hasvk, GMBUS, i965-va).
* `dev-log/70-wifi-chip/plan.md`: the form to copy.

## Facts to pin down in the code

* rach2/ layout: PCI, device memory maps, interrupts, framebuffer console, sources.mk, systab.c, syscalls.md.
* tests/: host test form, tests/run, suites.
* man/: page layout.
* Lenovo facts: dev-log/70-wifi-chip/lenovo.md, linux/dmesg.txt (GPU PCI id, stolen memory, aperture, DMAR).
* Upstream: build/research/src/linux (i915 haswell), mesa (crocus, hasvk), libdrm (i915_drm.h).

## Decisions taken while drafting

(filled in as the draft grows)

## Open points

(filled in as the draft grows)

## Facts gathered (round 1)

* Lenovo GPU: 8086:0a16 Haswell ULT GT2 (HD 4400), `dev-log/70-wifi-chip/linux/dmesg.txt:319-322`: BAR 0 0xc0000000 (4 MiB, 64-bit), BAR 2 0xb0000000 (256 MiB, pref), BAR 4 I/O 0x4000. Stolen memory 0xaba00000-0xaf9fffff (dmesg:125); RMRR 0xab800000-0xaf9fffff (dmesg:196). Linux: `i915 ... Found haswell/ult (device ID 0a16) integrated display version 7.00` (dmesg:681). DMAR unit 0 at 0xfed90000 covers only 00:02.0 (lenovo.md:84).
* Rach2 today: `rach2/sys/display/intelbl.c` already maps BAR 0's pages 0x48000, 0xC7000, 0xC8000 and drives the PCH PWM (BLC_PWM_PCH_CTL2 0xC8254, PP_CONTROL 0xC7204); the framebuffer console draws on the firmware's framebuffer at 0xb0000000 (the aperture!) write-combining (`display/Readme.md:116-118`, lenovo.md:316).
* `rach2/sys/pci.h`: pci_find_device_by_id, pci_bar_address64, pci_msi_enable (one vector), capability walk; no `_PRT` routing (lenovo.md:70 says INTx route is read from `_PRT` by hand). GPU interrupt: MSI (i915 uses MSI on HSW).
* MMIO region 128 MiB (`mm/Readme.md:42`, `arch/x86_64/cpu.h:132`); `map_mmio`, `map_mmio_typed` (PTE_WC); the MMIO region never frees a mapping (`mm/Readme.md:228`). The 256 MiB aperture cannot be mapped whole: windows (plan 75-3).
* MSI vectors: 16, ISR_MSI_FIRST 48 to ISR_MSI_LAST 63 (`arch/x86/isr.h:98-99`).
* Host tests: `tests/host/NAME_hosttest.c` with `host-deps:` lines; models like `ath9k_model.c`; sanitizers; form in `tests/host/Readme.md:111-125`. Suites: `tests/cases/*.json`, `tests/registry`, `tests/system/suites.md`.
* man: `man/manN/NAME.N` mdoc; `man/aliases`; `tools/verify/mancheck.c`.
* Manifests: `rach2/sources.mk` (C_SRC_SHARED, one file per line), `utils/programs.mk`.
* The machine skill: a trial boot of code that never ran on the Lenovo needs the worker's note on the issue (registers checked line by line against the reference, bounded waits, interrupts masked until ready, the host test running the very path) and the manager's go; `bootctl kernel FILE` arms one boot; silent, `mixer vol 1`; one task holds `build/flow/lenovo`.
* Fable round 3 (`round3/fable.md:39-52`): display part sizes (pipe 600, eDP 800, DP LT 600, HDMI 600, fitter 200, VBT 500 ...); no CRT on HSW ULT (`intel_display.c:7767`); without a VBT Linux assumes eDP on DDI A (`intel_bios.c` init_vbt_missing_defaults ~2909-2940); hasvk needs syncobj (`anv_device.c:1703`), timeline (`:1743`), Vulkan 1.2 (`:974-975`), incomplete warning (`:1616`); parser gen7 gate `i915_cmd_parser.c:961-976`; aliasing PPGTT `i915_pci.c:323, 393-396`.
* Codex round 3: IGD FLR advertised but not proof of display recovery; stolen memory reserved before the allocator; userptr only with invalidation; reset may cost more than the guilty context.
