# 78 intel: plan

Milestone 78 of the graphics stack, whose branch is `milestone/76-display`. The stack is milestones 76 to 83, planned together in [the approved plan](../76-graphics/plan.md) and [its task list](../76-graphics/tasks.md); this file gathers what concerns milestone 78. The research behind it is in [dev-log/76-graphics/](../76-graphics/research/Readme.md).

## The goal

The goal: on the Lenovo, the desktop, browsers and games run on the HD 4400 with Linux's conformance and within the gate of Linux's speed, at any mode of the panel or an HDMI monitor; H.264 video decodes on the GPU; a program that hangs the GPU loses its context, and after the reset of the hung engine (the whole GPU only if that fails, as Linux does on Haswell) the other programs go on, as on Linux.

| Task | What it builds | Waits on |
|---|---|---|
| 78-1 Safety net | The chipset's watchdog, an iTCO of version 2, free under Linux (Debian loads `iTCO_wdt` by itself, inactive, nowayout 0, and systemd armed it at 30 s and fed it every 15 s): one controlled expiry and return under Debian, then Rach2's own TCO driver, armed and fed by Rach2, and a forced expiry on Rach2 by trial boot, before any hang experiment relies on it; a register dump through /dev/mem, the pipe CRC, a capture tool | nothing |
| 78-2 Intel base | Registers, power wells, the global GTT, stolen memory reserved, forcewake | 76-1, 76-4; nothing of 78-1, a Lenovo task |
| 78-3 Buffers | Create, CPU and GTT maps (the 256 MiB aperture reached through windows, as Linux's io_mapping does, since the kernel's device window is 128 MiB), caching, tiling with the real swizzle, busy and wait, PRIME; eviction and rebinding of idle buffers, aperture accounting, purgeable buffers; userptr only with its invalidation rules; done when a working set larger than the aperture that fits in RAM runs, its contents checked after rebinding, and succeeds or fails as on Linux with the same workload | 78-2 |
| 78-4 Ring and first light | The render ring, status page, interrupts; one GPU write to memory before any display code | 78-2 |
| 78-5 Contexts and submission | Hardware contexts with their parameters (recoverable, priority), EXECBUFFER2 with relocations and fence arrays, the parameters Crocus and hasvk read, the command parser's version among them, without which hasvk refuses Haswell (`anv_device.c:1664-1671`); the checklist is the table of parameters and ioctls Linux answered on the Lenovo (`tonight/lenovo.md`), each marked required or a probe that may fail | 78-3, 78-4, 76-3 |
| 78-6 Reset and power | Hang detection, the reset of the hung engine with the whole-GPU reset as fallback, as Linux does on Haswell (`intel_reset.c:1473`), the reset statistics Crocus reads for GL robustness, the guilty context banned and the others' work replayed, measured against Linux with render and video work at once and with piglit's multisample `read-front` tests and `arb_copy_image-formats --samples=2`, which hung the GPU 9 times in the reference's rerun under Linux, each recovered by an engine reset (their commands and Linux's recovery kept); RC6 and frequency control with residency counters (Linux: 9960 ms of 10 s idle in RC6) | 78-5 |
| 78-7 ABI and fault backend | A test-only backend that completes work at once, so Crocus starts in QEMU through the real paths | 76-1 |
| 78-8 Page table and parser | The aliasing PPGTT and Linux's Haswell command parser; ordinary users submit only after this | 78-5 |
| 78-9 Display pipe | Readout and adoption of the firmware's state, atomic planes with X-tiling and the formats the hardware scans out (Haswell's primary plane takes XRGB8888 and no ARGB8888, measured: glmark2-drm needs `--visual-config a=0`), vblank and flip interrupts, cursor, backlight | 78-3, 76-11a |
| 78-10 Outputs and mode setting | Starting from what Linux reported on the Lenovo (the panel's one mode, its link of 1 lane at 2.7 Gb/s and 6 bits per colour, CDCLK 450 MHz, the panel's power timings, from `tonight/lenovo-out/debugfs/` (`i915_display_info`, `i915_cdclk_info`, `eDP-1-i915_panel_timings`), and the VBT and OpRegion bytes saved in `run2/step11/facts/`, which this task decodes): DDI, eDP with power sequencing, AUX and link training, DisplayPort and HDMI with infoframes, GMBUS with its pin map and bit-banging fallback for an HDMI monitor's EDID, the PLLs, the panel fitter (Linux's own gives FIFO underruns on pipes B and C through it, matched as Linux's), the VBT blocks, hotplug, the second pipe's power well, watermarks | 78-9 |
| 78-11 hasvk | The kernel pieces hasvk probes, from the trace of vkcube under Linux (timeline syncobj waits and transfers, sync-file import and export on dma-bufs, reset statistics per frame); vulkaninfo, vkcube and dEQP-VK's smoke set, off-screen first and presenting after 78-9 and the output work of 78-10 it needs; Chromium's WebGPU on hasvk with `--ignore-gpu-blocklist` from the test launcher (decision C), compute and render read back, and a WebGPU picture on the panel captured (59.9 FPS under Linux), its sandbox state recorded; hasvk at its default 1.2, with the 1.3 option tried only per program; dEQP-VK matched against Linux's own results on this Lenovo, never a clean pass: hasvk calls its Haswell support incomplete, reports conformance 0.0.0.0, and of every 50th case of VK-GL-CTS 1.4.7.1's `vk-default` (64,303) it passes 9,326, with 5 failures and 3 resource errors | 78-5, 78-8, 76-3 |
| 78-12 Accelerated browsers | Chromium's GPU process and Firefox's WebRender on Crocus, each process sandboxed as Debian's (decision D), or, if you take 76-20, Chromium's GPU process sandboxed on Crocus with WebGL and WebGPU working; an X11 GL and a Direct3D 9 check through Xwayland and Wine | 78-8, 78-9 |
| 78-13 Conformance against Linux | IGT's gem and KMS tests (with the i915 debugfs files they use: drop caches, wedged, the error state), piglit, dEQP-GLES, the WebGL suite and glmark2, each against the same run on Linux on this Lenovo, failures compared by case, every difference as Linux's or an exclusion you approve. Linux's results, with translation on: piglit `quick`, of 44,333 tests, 36,445 pass, 210 fail and 3 crash; dEQP-GLES2, GLES3 and GLES31 with 0, 1 and 14 failures; IGT's KMS tests 418 pass, 4 fail and 910 skip; the WebGL suite's CTS 2.0.1 without `deqp/`, taken with builds running beside, at 99.72 percent of subtests in Chromium and 97.95 in Firefox, counts that are not the whole-suite reference, which is taken again on an idle machine. The speed gate against translation-off Linux (change 2). Throughput and frame-time percentiles recorded apart, with run-to-run variation, versions, resolution and power settings | 78-10, 78-12, 76-18 |
| 78-14 Video decoding | The video ring and its command parser; Debian's i965 VA driver (MPEG-2, H.264, VC-1 and JPEG on Haswell); the decoder checked by frame hashes against software (equal under Linux); Firefox's hardware decoding confirmed by its own log; Chromium's as decision B says; drop counts against Debian's for each browser, each decoder named (decision B has Debian's) | 78-8; 78-12a for the browsers |

Done: 78-13's results with each difference as Linux's or an exclusion you approve, failures compared by case, and within the gate against translation-off Linux; 78-14's drop counts no worse than Debian's; the panel at each EDID mode with pipe CRCs matching Linux's, and HDMI's modes through IGT's EDID injection (a real monitor at your sitting after the release, 84-2); hotplug seen; a hung program's context lost, and the other programs going on or recovering exactly as they do on Linux on the same machine, piglit's two hang triggers among the cases; the parser's rejection cases; RC6 residency rising at idle; 78-3's working set beyond the aperture as on Linux, and memory pressure beyond RAM ending in a clean error, never corruption.

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

Each task below is an issue in the GitHub milestone `78 intel`, and each subtask a sub-issue under it; a wait is a blocker on GitHub. Faults found in other projects' code go in `upstream.md` beside this file, in the format [dev-log/upstream.md](../upstream.md) gives.

### 78-1a Watchdog driver and display tools

Builds: Rach2's driver for the chipset's TCO watchdog (version 2 on the Lenovo's Lynx Point LP, at I/O 0x1860, `run2/step11.md:36`; ICH9's in QEMU's q35) with Linux's `/dev/watchdog` interface; a register dump through `/dev/mem`, the pipe CRC and a scanout capture. Split from 78-1 because the code needs no Lenovo and starts at once; 78-1b holds the expiries.

* Files: new `rach2/sys/arch/x86/itco.c`, `itco.h`; `rach2/sys/arch/x86/sources.mk`; `rach2/sys/arch/x86/board.c` and `board.h` (an accessor for the profile's PMBASE and RCBA); a row in 76-1's device registry (`watchdog` at 10:130), not `devfs.c`; new `man/man4/watchdog.4`; `rach2/hardware.md` (a TCO row); new `tests/host/itco_hosttest.c` with a TCO model; new `tests/cases/machine.watchdog.json` and a `watchdog` line in `tests/registry`; new `utils/gfxtool.c`, `utils/programs.mk` (`SBIN_PROGS`), new `man/man8/gfxtool.8`.
* Waits on: 76-1 (code: the device registry).
* Labels: none.
* Subtasks:
  * 78-1a.1 The TCO driver for the `ich9` and `lynxpoint-lp` profiles (`board.c:40-45`): TCOBASE at PMBASE + 0x60, NO_REBOOT in GCS at RCBA + 0x3410, reload, the timeout in 0.6 s ticks, halt and start, the two-stage expiry, as Linux's `iTCO_wdt` version 2; every wait bounded; `tests/host/itco_hosttest.c` checks the register sequence and the expiry against the model.
  * 78-1a.2 `/dev/watchdog` at 10:130 with Linux's interface: open arms, write feeds, the magic close `V` disarms, nowayout 0 as Debian's, and `WDIOC_GETSUPPORT`, `GETSTATUS`, `GETBOOTSTATUS`, `KEEPALIVE`, `SETTIMEOUT`, `GETTIMEOUT`, `GETTIMELEFT` and `SETOPTIONS`; `man/man4/watchdog.4`; the `rach2/hardware.md` row.
  * 78-1a.3 A q35 case that arms the watchdog, stops feeding it, and reads QEMU's watchdog action through QMP; its author runs it once.
  * 78-1a.4 `utils/gfxtool.c`: `dump` prints Haswell's display, power, forcewake, GTT and ring registers by name through `/dev/mem` (BAR0 at 0xc0000000 is below 4 GiB, which `/dev/mem` covers, `man/man4/mem.4`); `crc` reads the pipe CRC for N frames and restores its control; `capture` reads the primary plane's surface, stride and format and writes a PPM from the aperture; every wait bounded; it refuses any GPU but 8086:0a16 and a GPU a driver owns.
  * 78-1a.5 `man/man8/gfxtool.8`, the program in `SBIN_PROGS`; its author runs it once on QEMU's std-vga to see the refusal.
* Done: the TCO host test and the q35 case pass, and gfxtool refuses every GPU but the HD 4400.

### 78-1b Watchdog and tools on the Lenovo

Builds: the proof that the watchdog brings the Lenovo back, before any hang experiment relies on it: one controlled expiry under Debian on p5, then Rach2's driver armed, fed and forced to expire on a trial boot; and gfxtool run on the firmware's display state, whose dump becomes the host model's fixture.

* Files: new `tests/fixtures/lenovo-gpu/firmware-dump.txt`; the record in `dev-log/78-intel/`.
* Waits on: 78-1a (code).
* Labels: `lenovo`.
* Subtasks:
  * 78-1b.1 Prepare the Debian part: p5 booted through GRUB's one-shot entry; `wdctl` read; systemd's runtime watchdog turned off for the session; `/dev/watchdog` opened and left unfed; the expected path back (firmware reset, GRUB's default entry, the installed Rach2). A Debian boot of its own, since the expiry ends the session by a reset. The `machine` skill has Linux leave by `rtcwake -m off -s 120` because a warm reboot once left the firmware unable to boot (`machine/SKILL.md:43`); the expiry is the chipset's own reset and the event under test, so the step records whether the firmware boots after it, and a failure to come back is this task's finding, reported at once.
  * 78-1b.2 Prepare the Rach2 part: the review note for `itco.c` and the manager's go; `bootctl kernel FILE`; after boot, gfxtool's dump, CRC and capture on the firmware's display, then the watchdog armed at 30 s, fed for two minutes, `GETTIMELEFT` read, feeding stopped; what passes is a reset within the timeout and the installed system back. Hand back `owner-only`.
  * 78-1b.3 The Lenovo step (Mac loop), silent: the Debian expiry, then the Rach2 trial, `mixer vol 1` after each boot; times to return recorded; the dump committed with the step's record as the fixture, and the CRC and capture compared with Linux's dumps of 80-2 where they exist.
* Done: both expiries brought the Lenovo back unattended, and the firmware state's dump is in the tree.

### 78-2a Driver skeleton and chip model

Builds: the Intel driver's skeleton: the Haswell register header, the hardware boundary through which every register access goes (so that the host model and the fault backend can stand in for the chip), the PCI identity `i915` for 8086:0a16 through 76-4, and a host model of the HD 4400 seeded with the Lenovo's facts. Split from 78-2 so that 78-7, 78-10a and 78-2b can start from it at once.

* Files: new `rach2/sys/drm/i915/sources.mk`, `i915.h`, `i915_reg.h`, `i915_hw.h`, `i915_pci.c`, `Readme.md`; the include of i915's manifest in `rach2/sys/arch/x86/sources.mk` beside 76-1's `pc.mk`, into `PC_C_SRC`, since the driver is PC-only and `check-arch` compiles shared files for ARM, and the prerequisite in `rach2/guest.mk:63`; new `man/man4/i915.4`; new `tests/host/i915_model.c` (the model's core and its table of blocks), `i915_model.h`, `i915_lenovo.h`, `tests/host/i915_pci_hosttest.c`; `rach2/hardware.md` (the HD 4400 row).
* Waits on: 76-1 (code: the interface header and identity table), 76-4 (code: DRM device registration). The model needs only the ids, BARs and stolen range, which `dev-log/70-wifi-chip/linux/dmesg.txt:190-200` and `:319-322` hold; the VBT and OpRegion bytes are 78-10a's.
* Labels: none.
* Subtasks:
  * 78-2a.1 `i915_reg.h` from Linux 6.12's register headers for the blocks 78 uses, Haswell's values only, each block naming its source file; it compiles for i686 and x86-64.
  * 78-2a.2 `i915_hw.h` and its MMIO implementation over BAR0 (4 MiB at 0xc0000000 on the Lenovo, `dev-log/70-wifi-chip/linux/dmesg.txt:319-322`) mapped uncached with `map_mmio_typed`; every wait takes a bound and returns a named error.
  * 78-2a.3 `i915_pci.c`: bind 8086:0A16 and the Haswell ids Linux 6.12 gives the same device data; bus mastering and MSI on; the DRM device registered as `i915` with Linux's version and features through 76-4. `intelbl.c` keeps the backlight until 78-9.
  * 78-2a.4 The model: `tests/host/i915_model.c` implements `i915_hw.h` over a register file that later tasks extend, seeded from `i915_lenovo.h` (ids, BARs, stolen memory at 0xaba00000 to 0xaf9fffff; 78-10a adds the VBT and OpRegion); `i915_pci_hosttest.c` binds the driver to it; registered in `tests/cases/host.host.json`. Each later task adds its registers in a block file of its own, registered in the core's table, so that tasks that run at once never edit one model file: `i915_model_base.c` 78-2b, `i915_model_ring.c` 78-4a then 78-8a, `i915_model_reset.c` 78-6a, `i915_model_rc6.c` 78-6b, `i915_model_pipe.c` 78-9, `i915_model_sink.c` 78-10b, `i915_model_ddi.c` 78-10c; 78-4b edits the block that holds a difference, after that block's owner.
  * 78-2a.5 The Readme (summary, Design with the hardware boundary and the lock order, Limits, a section per task), `man/man4/i915.4` with a section per ioctl family, and the `rach2/hardware.md` row.
* Done: the driver binds to the model, and every register access goes through `i915_hw.h`.

### 78-2b Intel base

Builds: forcewake, power wells, the global GTT and stolen memory reserved, so later parts can touch the GT and map memory.

* Files: new `rach2/sys/drm/i915/i915_uncore.c`, `i915_power.c`, `i915_gtt.c`, `i915_stolen.c`; new `tests/host/i915_model_base.c` (the model's forcewake, power wells and GTT); new `tests/host/i915_base_hosttest.c`.
* Waits on: 78-2a (code).
* Labels: none.
* Subtasks:
  * 78-2b.1 Forcewake on Haswell (`FORCEWAKE_MT`, its ack, a reference count, a bounded wait with a named error) and the GT FIFO's free entries; host test with a slow and a stuck ack.
  * 78-2b.2 Power wells (`HSW_PWR_WELL_CTL` request and state), the display well never turned off while the firmware's pipe runs; host test.
  * 78-2b.3 Stolen memory from BDSM, checked against the RMRR (0xab800000 to 0xaf9fffff), reserved from the frame allocator before anything else can take it; host test with the Lenovo's values.
  * 78-2b.4 The GGTT: Haswell's PTE format and cache bits, a scratch page, insert and clear ranges, and the firmware's entries for its framebuffer kept, since the console draws there through the aperture at 0xb0000000 (`rach2/sys/display/Readme.md`); host test.
  * 78-2b.5 The Readme's base section: forcewake rules, stolen memory, the GTT's layout.
* Done: the base host test passes, a stuck forcewake ack fails by name, and the firmware's GTT entries stay as they were.

### 78-3a Buffers

Builds: GEM objects: create, CPU and GTT maps (the 256 MiB aperture reached through windows, as Linux's io_mapping does, since the kernel's device window is 128 MiB, `cpu.h:148-149`), caching, tiling with Haswell's swizzle and fences, busy and wait, PRIME. Split from 78-3 because eviction, purging and userptr (78-3b) need the memory hooks of Problems 9 and nothing else waits for them.

* Files: new `rach2/sys/drm/i915/i915_gem.c`, `i915_gem_mman.c`, `i915_gem_tiling.c`, `i915_vma.c`, `i915_gem_prime.c`; new stub `i915_gem_evict.c` (fails a bind that does not fit, for 78-3b); new `tests/host/i915_gem_hosttest.c`; new `tests/i915gemprobe.c`.
* Waits on: 78-2b (code), 76-2 (code: device mappings, windows and revocation), 76-4a, 76-4b (code: handles and PRIME), 78-7 (code: the fault backend for the QEMU check).
* Labels: none.
* Subtasks:
  * 78-3a.1 Objects from 76-2's page source, handles, close, PREAD and PWRITE with cache flushes where the object is not coherent; host test.
  * 78-3a.2 SET_DOMAIN, SW_FINISH, SET_CACHING and GET_CACHING as Linux on an LLC part; host test.
  * 78-3a.3 MMAP_OFFSET with WB and WC, and GTT maps through a fault handler that binds the object and maps aperture pages through a window, revoked on unbind; host test with more objects than the windows hold.
  * 78-3a.4 X and Y tiling, the swizzle mode read as Linux's `i915_gem_detect_bit_6_swizzle`, fence registers for GTT maps of tiled objects; host test of the detiling arithmetic against Linux's tables.
  * 78-3a.5 BUSY, WAIT with timeouts, PRIME export and import through 76-3; host test.
  * 78-3a.6 `tests/i915gemprobe.c` on the fault backend in QEMU: a tiled object written through a GTT map and read back through a CPU map; its author runs it once; the man page's buffer section.
* Done: the GEM host test passes, and on the fault backend a tiled object written through a GTT map reads back right through a CPU map.

### 78-3b Eviction, purgeable buffers and userptr

Builds: eviction and rebinding of idle buffers, aperture accounting, purgeable buffers given up under memory pressure, and userptr with its invalidation rules; done when a working set larger than the aperture that fits in RAM runs, its contents checked after rebinding.

* Files: `rach2/sys/drm/i915/i915_gem_evict.c` (from 78-3a's stub); new `i915_gem_shrinker.c`, `i915_gem_userptr.c`; `i915_gem.c` (MADVISE, GET_APERTURE) after 78-3a; `tests/i915gemprobe.c` after 78-3a; new `tests/host/i915_evict_hosttest.c`; `rach2/sys/mm/` only if 76-2 does not provide the hooks (Problems 9).
* Waits on: 78-3a (code), 76-2 (code: the shrinker registration and the address-space change notifier).
* Labels: none.
* Subtasks:
  * 78-3b.1 Eviction: idle bound objects in LRU order evicted for a bind that does not fit the mappable aperture or the GGTT, rebound on next use, pinned and active objects never evicted; host test with a working set three times the aperture and its contents checked.
  * 78-3b.2 GET_APERTURE's accounting; host test.
  * 78-3b.3 MADVISE and purging of DONTNEED objects from a shrinker that memory pressure calls before an allocation fails (`rach2/sys/mm/pressure.h:7-10` reclaims only the page and buffer caches today); host test under pressure.
  * 78-3b.4 USERPTR: pages pinned on bind and released when the process unmaps, changes protection, forks or exits, bound VMAs revoked first; the unsynchronized flag refused without privilege as Linux; host test.
  * 78-3b.5 i915gemprobe rows on the fault backend: a working set beyond the aperture, a purge, userptr, and pressure beyond RAM ending in ENOMEM; its author runs them once; the Readme's memory section.
* Done: on the fault backend a working set larger than the aperture runs with its contents checked, and pressure beyond RAM ends in a clean error.

### 78-4a Ring, interrupts and first light

Builds: the render ring and the blitter ring (IGT's gem tests use both), the hardware status page, MSI and the GT interrupts, and first light: one `MI_STORE_DWORD_IMM` from the render ring to memory before any display code. Split from 78-4 so that the Lenovo trial (78-4b) holds no code.

* Files: new `rach2/sys/drm/i915/i915_ring.c`, `i915_irq.c` (every GT and display source with a handler table), `i915_firstlight.c`; new stub `i915_ppgtt.c` with `i915_ppgtt_enable_ring`, called at ring start, for 78-8a; new `tests/host/i915_model_ring.c` (the model's rings and interrupts); new `tests/host/i915_ring_hosttest.c`; a `rach2/cmdline.md` row.
* Waits on: 78-2b (code).
* Labels: none.
* Subtasks:
  * 78-4a.1 The status page in the GGTT, ring buffers bound in the GGTT, ring start with `RING_VALID` and a bounded wait for idle; host test.
  * 78-4a.2 One MSI vector; GT IIR read and acknowledged; IMR and IER for the render and blitter user interrupts; every other source masked until its owner registers; spurious interrupts counted; host test.
  * 78-4a.3 Breadcrumbs: `MI_STORE_DWORD_INDEX` to the status page and `MI_USER_INTERRUPT`; waiters woken from the interrupt; host test.
  * 78-4a.4 First light under a boot option (`rach2.i915.firstlight`): a value written to a GGTT page by the render ring and read back, logged as `i915: first light: ok` or a named failure; every wait bounded; host test end to end on the model.
  * 78-4a.5 The Readme's rings and interrupts section and the `rach2/cmdline.md` row.
* Done: first light runs end to end on the model, and every interrupt source but the rings' is masked.

### 78-4b First light on the Lenovo

Builds: the first trial of new Intel code on the Lenovo, silent: forcewake, power wells, the GTT, stolen memory and one GPU write, with the firmware's display left as it was.

* Files: the record in `dev-log/78-intel/`; the model block that holds a difference from the hardware, after that block's owner.
* Waits on: 78-2b, 78-4a (code), 78-1b (evidence: the watchdog proven).
* Labels: `lenovo`.
* Subtasks:
  * 78-4b.1 The review note for `i915_uncore.c`, `i915_power.c`, `i915_gtt.c`, `i915_stolen.c`, `i915_ring.c` and `i915_irq.c`, and the manager's go.
  * 78-4b.2 Prepare: a kernel whose init arms the watchdog before i915 probes; first light by boot option; gfxtool's dump before and after; what passes (`i915: first light: ok`, the console unchanged on screen, no FIFO underrun); why it cannot hang the machine (bounded waits, the watchdog). Hand back `owner-only`.
  * 78-4b.3 The Lenovo step (Mac loop): one silent trial boot, logs and dumps kept; a difference from the model is fixed in this task and the model updated.
* Done: first light passes on the Lenovo with the firmware's display unchanged.

### 78-5a Contexts, requests and parameters

Builds: hardware contexts with their parameters, requests and their retirement, and the answers Crocus and hasvk read: GETPARAM, I915_QUERY and GET_RESET_STATS, from the table Linux answered on the Lenovo (`tonight/lenovo.md:63-65`), each row required or a probe that may fail. Split from 78-5 because contexts and parameters do not need EXECBUFFER2 and 78-6b, 79-6c and 78-5b start from them.

* Files: new `rach2/sys/drm/i915/i915_gem_context.c`, `i915_request.c`, `i915_getparam.c` (GETPARAM, I915_QUERY, GET_RESET_STATS); new `tests/host/i915_ctx_hosttest.c`; the man page's context section.
* Waits on: 78-3a, 78-4a (code), 76-3 (code: syncobjs).
* Labels: none.
* Subtasks:
  * 78-5a.1 Hardware contexts on the render ring: the image size from `CXT_SIZE`, `MI_SET_CONTEXT` on switch, the default context; host test.
  * 78-5a.2 CONTEXT_CREATE (with extensions), DESTROY, GETPARAM and SETPARAM for recoverable, priority, bannable, GTT_SIZE and NO_ZEROMAP, refused with Linux's errors where Linux refuses; host test.
  * 78-5a.3 Requests: creation, sequence numbers, retirement from the interrupt, waits with a timeout; host test.
  * 78-5a.4 GETPARAM and I915_QUERY from the table Linux answered (CHIPSET_ID 0x0a16, REVISION 11, HAS_GPU_RESET 2, ALIASING_PPGTT 1, ... `tonight/lenovo.md:63`), CMD_PARSER_VERSION left unanswered until 78-8b; host test row by row.
  * 78-5a.5 GET_RESET_STATS answers for every context from the start, with counts that 78-6a fills, since hasvk declares its device lost when the call fails (`vulkan_hasvk/anv_device.c:2875-2879`); host test.
* Done: every GETPARAM and I915_QUERY row Linux answered answers the same on the model, and GET_RESET_STATS answers for every context.

### 78-5b Submission

Builds: EXECBUFFER2 with relocations, fence arrays and in and out fences on the render and blitter rings; Crocus starts in QEMU on the fault backend through the real paths. Until 78-8 lands, a batch from a process without privilege goes to the parser's stub, which refuses it.

* Files: new `rach2/sys/drm/i915/i915_gem_execbuffer.c`; new stub `i915_cmd_parser.c` (`i915_cmd_parser_check` refuses every unprivileged batch) for 78-8b; new `tests/i915execprobe.c`; new `tests/host/i915_exec_hosttest.c`; the man page's submission section.
* Waits on: 78-5a (code), 78-7 (code: the fault backend), 76-3 (code).
* Labels: none.
* Subtasks:
  * 78-5b.1 Object lookup, binding through 78-3a's VMAs, relocations with presumed offsets, NO_RELOC, HANDLE_LUT and BATCH_FIRST; host test with Linux's relocation cases.
  * 78-5b.2 Batch start on the render and blitter rings, the request emitted, the out-fence; host test.
  * 78-5b.3 Fence arrays of syncobjs, an in-fence and an out-fence as sync files; host test.
  * 78-5b.4 Unprivileged batches through `i915_cmd_parser_check`, refused by the stub; host test.
  * 78-5b.5 `tests/i915execprobe.c` on the fault backend: Crocus initializes (`eglinfo -B -p surfaceless` names it), submissions complete, and injected faults (EIO, ENOSPC, a hung request) reach Crocus as Linux's errors; its author runs it once.
* Done: on the fault backend Crocus starts and its submissions complete through Rach2's EXECBUFFER2, and injected faults return Linux's errors.

### 78-5c Submission on the Lenovo

Builds: the first trial of submission on the Lenovo, as root and silent: IGT's gem basics, a working set beyond the aperture, Crocus off-screen.

* Files: new `tests/graphics/igt-gem-trial.txt` (the case list); the record in `dev-log/78-intel/`.
* Waits on: 78-3b, 78-5b (code), 76-13b (code: IGT's tests), 76-15b (code: the trial root's slot), 76-15c (evidence: the data partition on the Lenovo), 78-4b (evidence).
* Labels: `lenovo`.
* Subtasks:
  * 78-5c.1 The review note for the new submission and memory paths, and the manager's go.
  * 78-5c.2 A trial root with IGT's tests, piglit and Crocus from 76-13; the list: `gem_basic`, `gem_create`, `gem_close_race`, `gem_exec_basic`, `gem_exec_store`, `gem_mmap`, `gem_mmap_gtt`, `gem_tiled_blits`, `gem_ctx_param`, `gem_reset_stats`, `gem_busy`, `gem_wait`, `gem_userptr_blits` basic, `prime_self_import`; Crocus off-screen by `eglinfo` and a readback; Linux's results for the same cases from step 9. Hand back `owner-only`.
  * 78-5c.3 The Lenovo step (Mac loop): one silent trial boot as root, results compared by case.
* Done: each case of the list passes or fails as Linux's on the Lenovo, and Crocus draws right off-screen.

### 78-6a Hang detection and reset

Builds: hang detection, the reset of the hung engine with the whole-GPU reset as fallback, as Linux on Haswell (plan.md, 78's table, 78-6), the reset statistics, the guilty context banned and the others' work replayed.

* Files: new `rach2/sys/drm/i915/i915_hangcheck.c`, `i915_reset.c`, `i915_gpu_error.c` (the error state captured at a hang); `i915_gem_context.c` (ban and counts) after 78-5a; new `tests/host/i915_model_reset.c` (the model's reset); new `tests/host/i915_reset_hosttest.c`; new `tests/i915resetprobe.c`.
* Waits on: 78-5b (code).
* Labels: none.
* Subtasks:
  * 78-6a.1 Hang detection: a request whose sequence number and ACTHD do not move within Linux's period is hung; host test.
  * 78-6a.2 Engine reset through GDRST's engine bit with a bounded wait, the ring, status page and context restored; the whole-GPU reset when that fails; host test with a stuck reset.
  * 78-6a.3 The guilty context marked and banned after Linux's count, its requests failed with EIO, innocent requests replayed, GET_RESET_STATS counts as Linux's; host test.
  * 78-6a.4 The error state (rings, active requests, batch heads) captured at a hang for 78-13a's files.
  * 78-6a.5 `tests/i915resetprobe.c` on the fault backend: an injected hang, the reset, the ban and the replay; its author runs it once; the Readme's reset section.
* Done: on the fault backend an injected hang costs only the guilty context, its counts show it, and the other contexts' work completes.

### 78-6b RC6 and frequency

Builds: RC6 and frequency control with residency counters (Linux: 9960 ms of 10 s idle in RC6). Split from 78-6 because it needs only the ring and interrupts, not submission.

* Files: new `rach2/sys/drm/i915/i915_rc6.c`, `i915_rps.c`; the `/sys` files Linux has (`gt_cur_freq_mhz`, `gt_min_freq_mhz`, `gt_max_freq_mhz`, `gt_RP0_freq_mhz`, `gt_RP1_freq_mhz`, `gt_RPn_freq_mhz`, `power/rc6_residency_ms`) through 76-9's attribute interface; new `tests/host/i915_model_rc6.c` (the model's RC6 and pcode mailbox); new `tests/host/i915_power_hosttest.c`.
* Waits on: 78-4a (code), 76-9 (code: the `/sys` attribute interface).
* Labels: none.
* Subtasks:
  * 78-6b.1 RC6 with Linux's thresholds and the residency counter read in milliseconds; host test.
  * 78-6b.2 Frequency through the pcode mailbox (bounded) and RPNSWREQ, the up and down interrupts handled from 78-4a's table; host test.
  * 78-6b.3 The `/sys` files and their section in `man/man4/i915.4`.
  * 78-6b.4 The files read on the fault backend in QEMU by their author once.
* Done: on the model RC6 is entered at idle and frequency requests stay in the limits, and the files have Linux's names.

### 78-6c Reset, power and the parser on the Lenovo

Builds: a silent trial: piglit's two hang triggers, which hung the GPU 9 times under Linux, each recovered by an engine reset; RC6 residency at idle; the parser's rejection cases; ordinary users' submissions. Render and video hung at once is in 78-14b, since the video ring comes later.

* Files: the record in `dev-log/78-intel/`.
* Waits on: 78-6a, 78-6b, 78-8a, 78-8b (code), 78-5c (evidence).
* Labels: `lenovo`.
* Subtasks:
  * 78-6c.1 The review note for GDRST, the pcode mailbox and the RC6 registers, and the manager's go.
  * 78-6c.2 Prepare: piglit's multisample `read-front` tests and `arb_copy_image-formats --samples=2` with the reference's commands, other programs running beside; `gem_reset_stats`, `gem_exec_parse` as an ordinary user, an ordinary user's Crocus readback; a 10 s idle residency read; what passes is each hang recovered with only the guilty context lost, as Linux. Hand back `owner-only`.
  * 78-6c.3 The Lenovo step (Mac loop), silent: a trial boot of its own, since the hangs take the GPU; `mixer vol 1` after the boot.
* Done: each hang recovers as on Linux, RC6 residency rises at idle, and every parser rejection case matches Linux's.

### 78-7 ABI and fault backend

Builds: a test-only backend behind `i915_hw.h` that completes every batch at once and injects faults, so Crocus starts in QEMU through Rach2's real paths; it proves the interfaces and draws nothing (the plan's decision 12).

* Files: new `rach2/sys/drm/i915/i915_fake.c`; its binding under `rach2.i915fake` (a `rach2/cmdline.md` row); new `tests/host/i915_fake_hosttest.c`; new `tests/cases/machine.i915fake.json` and a line in `tests/registry`; the Readme's tests section.
* Waits on: 78-2a (code: `i915_hw.h`), 76-4 (code), 76-9 (code: a DRM device's PCI identity in `/sys`, which Mesa reads; Problems 8).
* Labels: none.
* Subtasks:
  * 78-7.1 The fake chip: a register file, a GTT, rings whose head reaches the tail at once, sequence numbers written and interrupts raised; host test.
  * 78-7.2 Binding: under `rach2.i915fake`, the driver binds to a QEMU PCI device chosen for it and reports 8086:0A16 in `/sys` through 76-9, so that libdrm's `drmGetDevice2` and Mesa's loader see Haswell.
  * 78-7.3 Fault injection through debugfs: EIO on submit, a hang, ENOSPC on bind, a lost interrupt.
  * 78-7.4 A `machine.i915fake` case boots with the backend and checks that Mesa's loader names `crocus`; its author runs it once.
* Done: on QEMU with `rach2.i915fake`, libdrm reports 8086:0a16 and Mesa's loader picks Crocus.

### 78-8a Aliasing PPGTT

Builds: the aliasing PPGTT, as Linux on Haswell: one PPGTT shared by every context, mirroring the GGTT's bindings. Split from 78-8 because it and the parser touch different files and can run at once.

* Files: `rach2/sys/drm/i915/i915_ppgtt.c` (from 78-4a's stub); `tests/host/i915_model_ring.c` (the PPGTT registers, after 78-4a); new `tests/host/i915_ppgtt_hosttest.c`.
* Waits on: 78-5b (code).
* Labels: none.
* Subtasks:
  * 78-8a.1 The page directory in the GGTT and its tables; bindings mirrored into it.
  * 78-8a.2 `PP_DIR_DCLV` and `PP_DIR_BASE` for each ring at start and after a reset, through `i915_ppgtt_enable_ring`; `GFX_MODE`'s PPGTT bit.
  * 78-8a.3 Host test: a batch's addresses resolve through the PPGTT to the same pages as the GGTT's.
  * 78-8a.4 The Readme states that all programs share one GPU address space on Haswell, as on Linux (the plan's decision 4).
* Done: on the model every ring runs with the aliasing PPGTT enabled, after a reset too.

### 78-8b Command parser

Builds: Linux's Haswell command parser for the render and blitter rings, after which ordinary users submit; its version answered through GETPARAM, without which hasvk refuses Haswell (`anv_device.c:1664-1671`).

* Files: `rach2/sys/drm/i915/i915_cmd_parser.c` (from 78-5b's stub); `i915_getparam.c` (CMD_PARSER_VERSION 10, Linux 6.12's `i915_cmd_parser.c:1623`) after 78-5a; new `tests/host/i915_parser_hosttest.c`; the man page's parser section.
* Waits on: 78-5b (code).
* Labels: none.
* Subtasks:
  * 78-8b.1 Linux 6.12's command tables and register allowlists for the render and blitter rings on Haswell.
  * 78-8b.2 The parse: the batch copied to a shadow, chained batches followed as Linux, privileged commands and registers refused.
  * 78-8b.3 GETPARAM CMD_PARSER_VERSION.
  * 78-8b.4 Host test with every `gem_exec_parse` case Linux rejects and accepts.
* Done: the host test rejects every case Linux rejects and accepts the rest, and GETPARAM reports version 10.

### 78-9 Display pipe

Builds: readout and adoption of the firmware's display state, atomic planes with X-tiling and the formats Haswell scans out (the primary plane takes XRGB8888 and no ARGB8888), vblank and flip interrupts, cursor, backlight, and the pipe CRC through debugfs.

* Files: new `rach2/sys/drm/i915/intel_display.c`, `intel_plane.c`, `intel_cursor.c`, `intel_vblank.c`, `intel_backlight.c`, `i915_display_debugfs.c`; `rach2/sys/display/intelbl.c` (its PWM code moves behind the connector, one owner of the PWM) and `man/man4/backlight.4`; new `tests/host/i915_model_pipe.c` (the model's pipe, planes and CRC); new `tests/host/i915_display_hosttest.c`.
* Waits on: 78-3a (code: framebuffers are GEM objects), 78-4a (code: the interrupts), 76-5 (code: atomic KMS), 76-11a (code: the console on DRM), 76-18 (code: the CRC files). It does not wait on 78-1b, a Lenovo task; its host test starts from Linux's readout and takes 78-1b's dump when that is in the tree.
* Labels: none.
* Subtasks:
  * 78-9.1 Readout of pipe A, transcoder EDP, DDI A, plane A (surface, stride, format, tiling), the fitter and the PPS into atomic state; host test seeded from Linux's `i915_display_info` and panel timings (`tonight/lenovo-out/debugfs/`), and from 78-1b's dump once it is committed.
  * 78-9.2 Adoption: the first commit keeps the mode, takes over the framebuffer, and 76-11's console moves without a blank; host test.
  * 78-9.3 Planes: the primary's formats as Linux lists them for Haswell (no ARGB8888), the sprite plane, linear and X-tiling, stride and offset checks; host test.
  * 78-9.4 Vblank and flip-done on each pipe, the frame counter, page flips at vblank with events and monotonic stamps; host test.
  * 78-9.5 The cursor plane and the backlight through the connector and `/sys/class/backlight/intel_backlight`; `intelbl.c` stands down when i915 binds; `backlight.4` updated.
  * 78-9.6 The pipe CRC through 76-18's files; the Readme and man page's display sections; on the fault backend, modetest lists the connector and the plane formats.
* Done: on the model the firmware's state is adopted without a mode set, and flips, vblank events, the cursor and the CRC work.

### 78-10a VBT and OpRegion

Builds: the VBT and OpRegion decode from the Lenovo's saved bytes (`run2/step11/facts/`). Split from 78-10 because it needs only the skeleton, starts early, and the outputs that follow are too large for one task (Problems 18).

* Files: new `rach2/sys/drm/i915/intel_bios.c`, `intel_opregion.c`; new `tests/host/intel_bios_hosttest.c`; `tests/host/i915_lenovo.h` (the VBT and OpRegion, after 78-2a); it reads the fixtures `tests/fixtures/lenovo-gpu/i915_vbt.bin` (6,144 bytes) and `i915_opregion.bin` (8,192 bytes), which the manager commits when the branch is cut (decision H's recommendation).
* Waits on: 78-2a (code).
* Labels: none.
* Subtasks:
  * 78-10a.1 The OpRegion found through ASLS, its header and mailboxes checked, a bad signature refused.
  * 78-10a.2 The VBT's blocks as Linux 6.12's `intel_bios.c`: general definitions, child devices and ports, LFP data and power timings, eDP, driver features; each block's size checked.
  * 78-10a.3 Linux's defaults without a VBT (eDP on DDI A).
  * 78-10a.4 Host test: the Lenovo's bytes decode to the ports, panel timings and eDP settings Linux reported (`tonight/lenovo-out/debugfs/`, `i915_vbt` and `eDP-1-i915_panel_timings`); the Readme's section.
* Done: the host test decodes the Lenovo's VBT to what Linux reported.

### 78-10b eDP and the panel

Builds: a full mode set of the panel: DDI, eDP with power sequencing, AUX and link training (1 lane at 2.7 Gb/s, 6 bits per colour), the panel fitter, CDCLK (450 MHz) and the watermarks of one pipe.

* Files: new `rach2/sys/drm/i915/intel_ddi.c`, `intel_dp.c`, `intel_dp_aux.c`, `intel_dp_link_training.c`, `intel_pps.c`, `intel_panel.c`, `intel_pfit.c`, `intel_cdclk.c`, `intel_wm.c`; new `tests/host/i915_model_sink.c` (the model's AUX sink and panel); new `tests/host/i915_edp_hosttest.c`.
* Waits on: 78-9, 78-10a (code).
* Labels: none.
* Subtasks:
  * 78-10b.1 AUX with bounded waits and retries; DPCD read; host test against a sink model.
  * 78-10b.2 Link training, clock recovery and channel equalization as Linux's; host test reaching 1 lane at 2.7 Gb/s.
  * 78-10b.3 The panel power sequencer with the VBT's timings; host test of the on and off order.
  * 78-10b.4 A full disable and enable of pipe A on eDP in Linux's register order; the fitter for the modes the connector lists; host test.
  * 78-10b.5 CDCLK readout and its rules; the watermarks of one pipe; the Readme's outputs section.
* Done: on the model a full mode set of the panel follows Linux's register sequence and link training reaches 1 lane at 2.7 Gb/s.

### 78-10c External outputs

Builds: HDMI and DisplayPort monitors: GMBUS with its pin map and the bit-banging fallback for an HDMI monitor's EDID, HDMI with infoframes, the PLLs, hotplug, the second pipe's power well and the watermarks of two pipes.

* Files: new `rach2/sys/drm/i915/intel_gmbus.c`, `intel_hdmi.c`, `intel_dpll.c`, `intel_hotplug.c`; `intel_dp.c` and `intel_wm.c` after 78-10b; `i915_power.c` after 78-2b; new `tests/host/i915_model_ddi.c` (the model's GMBUS, HDMI and PLL registers); new `tests/host/i915_hdmi_hosttest.c`.
* Waits on: 78-10b (code), 76-18 (code: EDID override and connector forcing).
* Labels: none.
* Subtasks:
  * 78-10c.1 GMBUS with the VBT's pin map and the bit-banging fallback; host test reading an EDID from a model.
  * 78-10c.2 HDMI on DDI B, C and D as the VBT says, with AVI and SPD infoframes; host test.
  * 78-10c.3 WRPLL and SPLL computed as Linux's; host test with a table of clocks against Linux's values.
  * 78-10c.4 External DisplayPort on the DDI ports through `intel_dp.c`.
  * 78-10c.5 Hotplug interrupts (long and short pulses, storm detection) and the second pipe's power well and watermarks; host test; IGT's `kms_hdmi_inject` path through 76-18's EDID override.
* Done: on the model an injected HDMI EDID's modes are set with Linux's PLL values, and hotplug reports connect and disconnect.

### 78-10d Display on the Lenovo

Builds: the display trial, which takes the screen: adoption at boot without a blank, the panel at each mode with pipe CRCs matching Linux's for one pattern, the fitter, the backlight, HDMI by IGT's EDID injection, hotplug.

* Files: new `tests/graphics/ref/lenovo-crc.txt` (Linux's CRCs); the record in `dev-log/78-intel/`.
* Waits on: 78-10b, 78-10c (code), 78-4b (evidence).
* Labels: `lenovo`.
* Subtasks:
  * 78-10d.1 The review note for the display, DDI, AUX, PPS and PLL registers, and the manager's go.
  * 78-10d.2 Prepare the Debian part: IGT's CRC of one pattern at each mode on Linux, kept as the reference; a Debian boot of its own, since the CRCs take the screen.
  * 78-10d.3 Prepare the Rach2 part: modetest at each mode with the CRC read; the fitter; backlight levels; `kms_hdmi_inject`; hotplug; IGT's KMS subset (`kms_addfb_basic`, `kms_atomic`, `kms_flip` basic, `kms_pipe_crc_basic`, `kms_cursor_crc` one size); a capture at adoption. Hand back `owner-only` with a "Lenovo notice:".
  * 78-10d.4 The Lenovo step (Mac loop): the Debian boot, then the Rach2 trial boot, `mixer vol 1` after each; a difference from Linux is fixed in this task.
* Done: the panel at each mode with CRCs matching Linux's, adoption without a blank, HDMI by injection and hotplug seen.

### 78-11a hasvk kernel pieces

Builds: the kernel pieces hasvk probes, from the vkcube trace under Linux (`tonight/lenovo.md:65`): timeline syncobj waits and transfers in EXECBUFFER2's extension, sync-file import and export on dma-bufs, `I915_REG_READ` of the timestamp, reset statistics each frame. Split from its Lenovo step (78-11c), since 78-11b, 80-5 and 83-2b need the code.

* Files: `rach2/sys/drm/i915/i915_gem_execbuffer.c` (the timeline fences extension) after 78-5b; new `i915_reg_read.c` (`I915_REG_READ` of the timestamp); `tests/host/i915_exec_hosttest.c` (timeline rows, after 78-5b). The parameters hasvk reads are already in 78-5a's table.
* Waits on: 78-5b, 78-8b (code: hasvk needs the parser's version), 76-3 (code: timeline syncobjs and sync-file export).
* Labels: none.
* Subtasks:
  * 78-11a.1 The timeline fences extension of EXECBUFFER2; host test.
  * 78-11a.2 The trace's ioctls checked one by one against Rach2, and `I915_REG_READ` for the timestamp; host test.
* Done: the host tests pass for every ioctl of the trace.

### 78-11c hasvk off-screen on the Lenovo

Builds: vulkaninfo and dEQP-VK's smoke set off-screen on the Lenovo, as root on a silent trial boot.

* Files: new `tests/graphics/deqp-vk-smoke.txt`; the record in `dev-log/78-intel/`.
* Waits on: 78-11a (code), 77-9 (code: VK-GL-CTS), 78-5c (`evidence`: submission proven on the Lenovo).
* Labels: `lenovo`.
* Subtasks:
  * 78-11c.1 The review note for the timeline path and `I915_REG_READ`, and the manager's go.
  * 78-11c.2 Prepare: `vulkaninfo`, dEQP-VK's smoke set and every 500th case of `vk-default` (Linux: 936 pass, 1 fail, 1 resource error of 6,431, `run2/step9/deqp/run.log`), compared by case. Hand back `owner-only`.
  * 78-11c.3 The Lenovo step (Mac loop): a silent trial boot of its own; `mixer vol 1` after the boot.
* Done: vulkaninfo matches Linux's on the Lenovo, and the smoke set and every 500th case match Linux's by case.

### 78-11b hasvk presenting and WebGPU

Builds: vkcube presenting, Chromium's WebGPU on hasvk with `--ignore-gpu-blocklist` from the test launcher (decision C), compute and render read back, and a WebGPU picture on the panel captured (59.9 FPS under Linux), its sandbox state recorded; hasvk at its default 1.2, with the 1.3 option tried only per program.

* Files: the launcher's switches in 76-17a's file, after 78-12a; the record in `dev-log/78-intel/`.
* Waits on: 78-11a, 78-12a (code: the browsers on the GPU and the launcher's file), 76-14 (code), 78-11c, 78-10d (`evidence`).
* Labels: `lenovo`.
* Subtasks:
  * 78-11b.1 Prepare vkcube under Weston, captured and its rate recorded beside Linux's under the same settings.
  * 78-11b.2 Prepare Chromium's WebGPU on hasvk: adapter named, compute and render read back, a picture captured, the sandbox state read from Chromium's diagnostics.
  * 78-11b.3 Hand back `owner-only` with a "Lenovo notice:"; a trial boot of its own, since it takes the screen; `mixer vol 1` after the boot.
  * 78-11b.4 The Lenovo step (Mac loop).
* Done: vkcube presents and WebGPU computes and draws right on hasvk with its adapter named, as on Linux.

### 78-12a Accelerated browsers: the launcher and the fixes

Builds: the launcher's GPU profile for Chromium's GPU process and Firefox's WebRender on Crocus, each process sandboxed as Debian's (decision D's recommendation), or, if the owner takes 76-20, Chromium's GPU process sandboxed on Crocus with WebGL and WebGPU working; the check scripts of the Lenovo step; and the kernel fixes the browsers need, each found and checked on the fault backend in QEMU. Split from its Lenovo step (78-12b), since 78-11b, 78-14b, 80-7b, 80-9 and 83-2b need the code.

* Files: the launcher's GPU profile in 76-17a's file, after 76-17a; new `tests/graphics/lenovo/browsers-gpu.sh` (the step's checks); kernel fixes as subtasks.
* Waits on: 78-8a, 78-8b, 78-9 (code), 76-14, 76-17a (code: the session and the browser launcher).
* Labels: none.
* Subtasks:
  * 78-12a.1 The launcher's GPU profile: Chromium's GPU process on Crocus with Debian's sandbox switches, Firefox with WebRender's hardware path. Check: on the fault backend in QEMU, Chromium's GPU process starts and names Crocus and Firefox's `about:support` names it; its author runs it once.
  * 78-12a.2 `browsers-gpu.sh`: WebGL's unmasked renderer, each process's sandbox from its diagnostics beside Debian's on the same binaries, `about:support`, an X11 GL program through Xwayland and a Direct3D 9 program through Wine on WineD3D's GL renderer, each read back. Check: its author runs it once on the fault backend, where the readbacks are not judged.
  * 78-12a.3 Each kernel fix the browsers need on the fault backend, a subtask filed as found.
* Done: on the fault backend both browsers start their GPU paths on Crocus with Debian's sandboxes, and the step's script runs end to end.

### 78-12b Accelerated browsers on the Lenovo

Builds: both browsers on Crocus on the Lenovo with each process's sandbox compared with Debian's, and the X11 GL and Direct3D 9 checks, on a trial boot that takes the screen.

* Files: the record in `dev-log/78-intel/`.
* Waits on: 78-12a (code), 78-6c (`evidence`: ordinary users' submissions proven on the Lenovo), 78-10d (`evidence`: the display).
* Labels: `lenovo`.
* Subtasks:
  * 78-12b.1 Prepare Chromium on Crocus: WebGL's unmasked renderer names Crocus, each process's sandbox read from its diagnostics and compared with Debian's on the same binaries.
  * 78-12b.2 Prepare Firefox on WebRender with Crocus, read from `about:support`.
  * 78-12b.3 Prepare an X11 GL program through Xwayland and a Direct3D 9 program through Wine on WineD3D's GL renderer, each read back.
  * 78-12b.4 Hand back `owner-only` with a "Lenovo notice:"; the Lenovo step (Mac loop), a trial boot of its own; `mixer vol 1` after the boot.
* Done: both browsers render on Crocus with each process sandboxed as Debian's, and the X11 GL and Direct3D 9 checks draw right.

### 78-13a i915 debugfs

Builds: the i915 debugfs files IGT's gem tests open: drop caches, wedged, the error state and the information files.

* Files: new `rach2/sys/drm/i915/i915_debugfs.c`, through 76-18's debugfs; the man page's debugfs section.
* Waits on: 78-3b (code: drop caches calls the shrinker), 78-6a (code: wedged and the error state), 76-18 (code).
* Labels: none.
* Subtasks:
  * 78-13a.1 `i915_gem_drop_caches` with Linux's flags.
  * 78-13a.2 `i915_wedged`, read and write, through 78-6a's reset.
  * 78-13a.3 `i915_error_state` read and cleared, and `i915_gpu_info`.
  * 78-13a.4 `i915_capabilities`, `i915_engine_info`, `i915_gem_objects` and `i915_frequency_info` in Linux's formats as far as IGT parses them.
  * 78-13a.5 On the fault backend, IGT's `gem_basic` and `gem_reset_stats` open the files; its author runs them once.
* Done: IGT's gem tests find these files and parse what they read.

### 78-13b Conformance against Linux

Builds: the Lenovo's conformance runs against Linux on the same machine: IGT's gem and KMS tests, piglit, dEQP-GLES (VK-GL-CTS 1.4.7.1), the WebGL suite and glmark2, failures compared by case, every difference as Linux's or an exclusion the owner approves; the speed gate against translation-off Linux (change 2, decision E). The whole comparison runs at the release's Lenovo step; this task builds it and proves each runner there.

* Files: new `tests/native/gpu.cases` and its name in `tools/etude/native_prepare.c:254`; new `tests/graphics/ref/lenovo/` (Linux's per-case results from `run2/step9/`); the exclusions list for the owner; the gate's settings.
* Waits on: 78-13a, 77-9, 76-22 (code). Its preparation does not wait on the Lenovo; its step runs last in the queue, after every other step of 78.
* Labels: `lenovo`.
* Subtasks:
  * 78-13b.1 The native case file with IGT gem and KMS, piglit `quick`, dEQP-GLES2, GLES3 and GLES31, the WebGL suite and glmark2 under Weston, each through its runner.
  * 78-13b.2 Linux's results turned into per-case expectations: piglit (`run2/step9/piglit/`), dEQP (`run2/step9/deqp/`), IGT (`run2/step9/igt/`), the WebGL suite.
  * 78-13b.3 The gate: glmark2 under Weston against translation-off Linux (1387 GL, 1379 GLES), the WebGL fill and ALU checks; the replays are 80's.
  * 78-13b.4 Prepare the Debian part: the WebGL suite retaken on an idle machine, as 78-13's row in plan.md's table requires, in a Debian boot of its own.
  * 78-13b.5 Prepare the Rach2 part: each runner on a small part, whole runs left to the release. Hand back `owner-only` with a "Lenovo notice:".
  * 78-13b.6 The Lenovo step (Mac loop): the Debian boot, then the Rach2 trial boot, `mixer vol 1` after each; each difference found goes on the exclusions list or is fixed in this task.
* Done: each runner judges by case on the Lenovo against Linux's stored results, and the exclusions list is with the owner.

### 78-14a Video ring and VA driver

Builds: the video ring and the video enhancement ring with their parser tables, and Debian's i965 VA driver (MPEG-2, H.264, VC-1 and JPEG on Haswell) with libva and vainfo in the root, which 76-13's list lacks (Problems 13).

* Files: `rach2/sys/drm/i915/i915_ring.c` (VCS and VECS) after 78-4a; `i915_cmd_parser.c` (their tables) and `i915_getparam.c` (HAS_BSD, HAS_VEBOX) after 78-8b; `vendor/gnu/debian/va.list` (new, its own list: `libva2`, `libva-drm2`, `i965-va-driver` 2.4.1, `vainfo`); the man page's video section.
* Waits on: 78-5b, 78-8b (code: the file order above), 76-13 (code); the downloads card (`decision`: the VA packages).
* Labels: `linux-host`.
* Subtasks:
  * 78-14a.1 VCS and VECS rings with their interrupts registered in 78-4a's table; host test.
  * 78-14a.2 Linux 6.12's parser tables for both; host test with their rejection cases.
  * 78-14a.3 GETPARAM rows for the new rings.
  * 78-14a.4 The VA packages pinned and installed, once the downloads card is answered.
  * 78-14a.5 On the fault backend, `vainfo` lists Haswell's decode profiles; its author runs it once.
* Done: on the fault backend vainfo lists Haswell's profiles through the i965 driver.

### 78-14b Video on the Lenovo

Builds: decoding on the GPU checked by frame hashes against software (equal under Linux); Firefox's hardware decoding confirmed by its own log; Chromium's as decision B's recommendation says; drop counts against Debian's for each browser with each decoder named; render and video work hung at once and recovered as on Linux (plan.md, 78's table, 78-6).

* Files: new `tests/graphics/ref/lenovo-video/` (Debian's drop counts and hashes); the record in `dev-log/78-intel/`.
* Waits on: 78-14a, 78-12a (code), 76-17a (code: the video runner), 78-12b (`evidence`: the browsers on Crocus).
* Labels: `lenovo`.
* Subtasks:
  * 78-14b.1 The review note for the VCS and VECS rings and their parser tables, and the manager's go.
  * 78-14b.2 Prepare frame hashes of H.264, MPEG-2, VC-1 and JPEG clips decoded through VA against a software decode.
  * 78-14b.3 Prepare Firefox and Chromium playback at 720p and 1080p with drops counted and each decoder read by name from the browser's log.
  * 78-14b.4 Prepare a hang on the render ring while the video ring decodes, recovered as on Linux.
  * 78-14b.5 Hand back `owner-only` with a "Lenovo notice:" (sound at `mixer vol 1`, the screen); the Lenovo step (Mac loop), a trial boot of its own; `mixer vol 1` after the boot.
* Done: decoded frames hash equal to software, drops are no worse than Debian's with each decoder named, and a hang during decoding recovers as on Linux.
