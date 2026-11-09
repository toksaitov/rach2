# Fixes to draft 6 after Codex's and Fable's reviews

Written by the chat on 2026-11-09 at 10:18 UTC+6 from `run2/codex-draft6.md` and `run2/fable-draft6.md`. Apply each to `build/flow/coordinator/graphics-research/plan.md` (draft 6). Line numbers are draft 6's as reviewed. `rulings.md` items 2 and 5 were corrected at 10:17; reread them.

## Statements

1. `:9` Crysis: "Linux's island flyby runs at 42.7 to 46.5 FPS under cage with sound off, against your 24; the whole first level and the route with working sound remain to be proved (80-2, 80-9)".
2. `:19` the run's start: 20:12.
3. `:22` native QEMU: "a worse graphics path than WSL on every graphics measure taken but one" (Windows laid down 2 GiB faster in step 1).
4. `:24` "the same exit status, and the same pixels where a probe reads them back".
5. `:30`, `:216`, `:250`: DXVK 3.1.1 and vkd3d-proton 3.1 "refuse Venus on both hosts tried; on hasvk they refuse the GPU itself". The measured 32-bit failure is "the mapping does not fit a 32-bit pointer"; `VK_EXT_map_memory_placed`, which 25.0.7 lacks, is the likely want, which 77-7 measures.
6. `:38`, `:369` KVM: the measured configuration (`shmem_enabled=always` faults; `never` or `deny` draws) and "the page-level cause is untraced", as `:289` says.
7. `:124`, `:195` sandbox: each process's measured Debian state (renderers and the storage service at seccomp 2 in their own namespaces; the network service at seccomp 0 in the host's; the GPU process unsandboxed), per ruling 2 as corrected. Decision D's early-start sentence: "the GPU process exits (testing PC's guest) or Chromium falls back to software (Lenovo)".
8. `:194` QMP's "no surface": by step 6's agent's words. Restore draft 5's reason for fixed size, warm-up and repetitions: browser fill numbers in QEMU varied by about 50 percent.
9. `:216` Chromium and the Venus adapter: "one visible difference is the GL device's id against Venus's; the cause is open, 80-2's trace".
10. `:236` the panel link, CD clock and power timings come from `tonight/lenovo-out/debugfs/` (`i915_display_info`, `i915_cdclk_info`, `eDP-1-i915_panel_timings`), not `run2/step11/facts/`.
11. `:239` the Lenovo WebGL figures: "CTS 2.0.1 without `deqp/`, taken with builds running beside"; they are counts, not the whole-suite reference.
12. `:309` 82-3: the VBT's ports and panel data for HDMI are "by i915's source"; the panel lit without it (measured).
13. `:325` 83-5: name each operation as measured: map and first touch; unmap and free; allocation, map and first touch (116.8 ms).
14. `:368` risk 14: QEMU 10.0.13's unmap finishes through its RCU thread, read in the source, not traced; its measured costs are 83-5's figures. No fixed cost per unmap.
15. `:453` futex: Rach2 has wait, wake and their bitset forms (`rach2/sys/proc/futex.c:465,472`); the missing operation is `futex_waitv`.
16. `:334` and `:442`: the trial root is accepted (`:144`); remove "if you agree" and "Put to you".

## Tasks and Dones

17. The plan approves goals, milestones, tasks, their order and Dones. Say once, in "After your approval", that each milestone's own plan (`dev-log/NN-name/plan.md`, the `milestone` skill) names each task's files and subtasks and goes to the owner for its own yes before that milestone opens, as AGENTS.md's Milestones section says. Then add a short section "Shared files", after "Order", naming the owner and the order of edits for the files several tasks touch: `rach2/sys/fs/procfs.c` and sysfs (76-9, then 76-19's files, then 80-10's sysfs part, which waits on 76-9); `rach2/sys/syscall_proc.c` (76-19 and 76-2's mapping calls; name the order); the mapping and pinning code, owned by 76-2, with 76-3, 77-3 and 81-1 waiting on it; and 76-19's new calls, which 79-2's coverage list and 79's i386 ABI carry.
18. Cross-milestone code: a task works on its own milestone's branch, so a task that needs another milestone's code waits for that milestone's release, or the provider moves into the consumer's milestone. Apply it: 76-21 consumes 80-1's GE-Proton package, so the package and its pin move into 76 (as a 76 task or in 76-13), and 80-1 keeps what is left. Check every "Waits on" that names another milestone's task the same way.
19. GE-Proton is pinned by SHA-256, as the fetcher checks (`vendor/gnu/fetch.sh:9,71`), with the SHA-512 the release publishes recorded beside it.
20. "Waits on" columns: 76-14 waits on the display, discovery, input and console tasks it uses; 77-4 on 77-7's chosen driver for the Venus parts; 80-3 on 80-1 and 77-2; Wine on virgl on the Proton package and virtio-gpu 3D; 82-5 on 81's VMX, interrupts and QEMU running. Split early preparation from integration where a whole-task wait would hold a worker.
21. 76-16: split into the profiles (at once) and the runners (last), so workers wait only on what they use.
22. 76's video Done: the reference is the Debian reference guest in the same QEMU, on llvmpipe with CPU decoding, both browsers, taken under 76-16. On the Lenovo, 80-2 adds Firefox's H.264 and VP9 with VA-API off (`media.ffmpeg.vaapi.enabled` false).
23. 76's WebGL Done: the reference pass counts are the Debian reference guest's on llvmpipe in the same QEMU, with the step 3-4 flakes (lavapipe's segfaults, the one GPU-process crash) named.
24. 76-21's Wine units: keep matching Linux. On Linux six units exit by themselves with code 5 before their summary; Rach2 matches each unit's exit and the checks before it, which is Linux's behaviour, not a harness interruption. dxgi on WineD3D stops making progress at the same check on Linux (`dxgi.c:8040`); it is matched by its progress, never by a clock. Say plainly that neither is a harness kill.
25. 76-15 runs first in 76, and 76's release waits on it; drop "the first release that ships a writable ext2 root", since the installed system already has one.
26. 76-20 (decision D, option 2): split its llvmpipe part (in 76) from its Crocus part (in 78, after 78-5 and 78-8), so taking it does not make 76 wait on 78.
27. 77-6: split WebGPU from the Direct3D acceptance, so WebGPU does not wait on 76-21, as draft 5 had it.
28. 78-1: Rach2's own TCO driver, arming and feeding it, and a forced expiry on Rach2, beside the Linux check under Debian.
29. 78-3: a measurable eviction Done: a working set larger than the GTT aperture that fits in RAM, its contents checked after rebinding, success or failure compared with Linux on the same workload.
30. 79's "covered or marked with a reason", and the open skip lists in 78 and 81: each difference matches Linux or is an exclusion the owner approves.
31. 80: SuperTuxKart's and 0 A.D.'s fixed replays are in the 10 percent gate's set of workloads. 80-4's Done notes "your goal; Linux's Weston frames the window at +32+32".
32. 80-2: mark it `owner-only` (the Lenovo). Add Linux's KVM exit costs and the KVM selftests' timing cases on the Debian on p5, so 81-7 has its Linux numbers early, as draft 5 had them. 78-10 decodes the saved VBT and OpRegion bytes.
33. 77-1's done part and 82-1: finished, so no card; move the facts into the milestone's introduction and keep only the rest as a task.
34. 82's introduction: why Debian's kernel translated DMA on this Lenovo without the option is untraced; a source read in 82-2.
35. 83: say whether a compositor restart on llvmpipe after the VM's death satisfies "the desktop goes on", and which task makes it so.

## Decisions

36. A: give each option its consequence for 77's Done (option 1: as written; option 2: Venus off-screen with readback, presenting out of 77's Done; option 3: no Venus presenting and no Direct3D on Venus in 77). Ask the module's load as A's second part: root loads it after each WSL start, or the owner allows it at boot. Restore the fallback's cost from draft 5: in the Debian VM `/dev/udmabuf` and `/dev/kvm` are root:kvm, so QEMU's account joins `kvm`.
37. E: the owner's ratification of the 10 percent gate (`:147`) is a fifth item; count it at `:15` and wherever the decisions are counted.
