Marks: [M] measured in brief/night run, [R] read in a participant answer or source it cites, [I] inferred.
**Where I was wrong**
- Muse (me): "Windows Venus does not exist today" was false [R]. Fable, Codex and the chat found WINQ-EMU. My buy-a-Linux-machine-first and write-the-port ordering falls with that error [I].
- I also assumed a whole WSL kernel, proposed a controlled RTX DDA attempt and an early hang test, and overvalued a Dozen vtest. Codex and Fable are better on all four, below [R/I].
- Fable understated the NVIDIA case [R]: issue 2 is not absence of evidence. It is the only NVIDIA report and it is a freeze (RTX 3070 Ti, vkcube). Codex and the chat state this correctly.

**1. udmabuf in WSL**
- First: the module, but Codex's way. Read-only check of `MODVERSIONS`, signing and exports first, then one exact-tag kernel build to get `Module.symvers` [R].
- That build is needed on either route [I], so trying the module first costs little extra. If it loads, no WSL restart ends every agent's session, and `rmmod` is the rollback [I].
- The gamble is narrow: WSL has `CONFIG_MODULES=y`, no signing or lockdown, but no `vmap_pfn` and no `Module.symvers` [M]. Fable's `vmap()` patch and symbol CRCs are the risk, and a module fault hits all WSL users [I].
- Switch to a whole kernel through `.wslconfig` if `insmod` fails on vermagic, CRC or missing exports, if the `vmap` change cannot be shown safe, or if reload at each WSL boot proves flaky [I].
- Make that switch in a named WSL restart window with the stock kernel kept for rollback. It needs no Windows reboot and leaves the other project's Hyper-V VMs alone [R].

**2. WINQ-EMU**
- Build alpha10 exactly as pinned first: matching QEMU and renderer forks, private prefix, no rebase [I]. Rebasing roughly 9,900 fork lines [R] before a baseline would destroy comparability with issue 2 and with the author's Intel B580 result.
- First run changes one variable at a time: `-cpu max` before `host` (Codex over Fable) [I], BIOS boot per the fork's README [R], Linux guest only, alone on the GPU.
- Stage the probes: vulkaninfo and extensions, a per-memory-type map and readback program, then vkcube [I]. A freeze at vkcube with no stage data before it tells nothing.
- If it freezes on the RTX, the very next step is the identical build, guest, flags and probe order on the Radeon, selected by explicit host ICD [I]. One run splits a NVIDIA-driver fault from a fork or transport fault.
- Radeon passes, RTX freezes: use the Radeon as the interim GPU bed and debug NVIDIA device-local host-visible memory (type 4) and Win32 sync handles, starting by hiding type 4 [R/I].
- Both freeze at the same stage: try `pc` against `q35` and `host` against `max` once each [R], then park alpha10. Do not debug it blind and do not rebase it yet [I].
- Parked means gfxstream on Windows next, as the transport with production Windows-host history [R], and the second-Linux-machine purchase put to the owner in parallel, not after long fork work [I].
- Rebase only if alpha10 works on at least one GPU here and the fault is localised to code upstream has since fixed [I]. Then a rebase is maintenance, not diagnosis.

**3. Discovery run, Linux as the proof**
One immutable Debian Linux reference image, fresh overlay per run, same Mesa, Wine, DXVK, vkd3d-proton and Chromium pins [I]. Linux passing in an environment proves that environment can; Rach2 stays out of the loop until then.
Workloads: V = vkcube 300 frames, D = DXVK D3D11 probe readback, K = vkd3d-proton D3D12 clear and compute, C = Crysis `-DEVMODE +map island +exec benchmark_gpu` on the ext2 disk, W = Chromium WebGPU adapter and compute readback.
Compared in every row: named adapter, API and extensions, exit status, frames completed, readback pixels, a presented screenshot, Crysis benchmark completion and counters. Speed is secondary [I].
| Environment | Linux guest / workloads | Decisive comparison | Pursue / drop gate |
|---|---|---|---|
| Guest lavapipe control, WSL KVM | Same image, lavapipe direct; V D K W | Workloads pass with no transport | Must pass first; else fix the fixture, not an environment |
| WSL virgl on RTX | Same; C via WineD3D, no V | GL pixels; Crysis completes | Already the daily GL bed [M]; makes no Vulkan claim |
| WSL Venus on lavapipe, udmabuf in | Same; V D K W | As control, plus swapchain and presented frame | Pass: daily Vulkan correctness bed [I]. W staying SwiftShader is expected [M] |
| Windows stock QEMU, ANGLE virgl | Same; C via WineD3D, V attempted | Renderer named; Venus capset present or absent | Pass on GL only; absent capset confirms no Venus in that DLL [M/R] |
| Windows WINQ Venus, RTX | Same; V D K C W | Discrete adapter, 1.3+, present works | All pass: primary real-GPU bed. Freeze: go to the row below |
| Windows WINQ Venus, Radeon | Same build and flags; V D K C W | Same stages as the RTX row | Pass alone: NVIDIA-specific fault. Both freeze: fork fault, park it |
| Windows gfxstream, conditional | Same image plus gfxstream guest driver; V D K C W | Same stages through the other transport | Run only if WINQ fails on both GPUs; pass replaces WINQ |
| Second Linux machine, KVM L1 | Same image; Venus on RADV or ANV; V D K C W | Upstream Venus on a Linux host, the gold reference [R] | Linux fails here too: workload is broken. Pass here only: buy it |
Crysis is left off the CPU rows on purpose [I]: the DXVK probe settles the same logic there, and a CPU-run benchmark only burns the shared 32 threads.

**4. Four candidates, in or out**
- DDA: out. Read-only survey only. Pro is refused with `0xC035001E` on the one client report [R], and a stuck dismount could force a reboot that interrupts the other project's VMs [M/I].
- Deliberate GPU hang: out of this run. It proves nothing about possibility, and a TDR resets the adapter for every user including WSL [R]. It belongs alone, later, after a path passes, on the Radeon or the second machine. Codex was right against me and the chat.
- Plain Hyper-V lavapipe host: in only as a conditional fallback, Codex's one real addition [R]. Use it if both the udmabuf module and the `.wslconfig` kernel are refused or fail. It is not a parallel track [I].
- Dozen behind Venus: out. Mesa 25.0.7 gives API 1.2, opaque fds and no fence fd or dma-buf [R], so it cannot prove the Vulkan 1.3 goal. A cheap vtest after the main gates, as a curiosity only.
