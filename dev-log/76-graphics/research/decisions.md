# The chat's notes on graphics

The chat's memory notes on the graphics work, the project's direction and the Crysis copies, as they stood on 2026-11-08, word for word. They record the owner's decisions in the chat and in the Codex thread.

## project-direction


On 2026-11-03 the owner said teaching is no longer the main goal, though Rach2 stays in the owner's courses and a book is planned. Two directions are open: a sovereign OS (the first Kyrgyz OS) or a university research sandbox for publishable OS ideas under full control of the code. The near goal: run the most basic 90% of consumer software with decent reliability, then pitch at local government startup events and seek investment (hardware, subscriptions, tokens, a test lab, people). Speed matters: "move fast and break things"; other agent-built OS projects exist but are behind.

Later on 2026-11-03 the owner named the graphics stack as "the next big thing", after a month spent on the test system, drivers and making the Lenovo solid as a development machine. Rach2 has no graphics stack yet: only the /fb framebuffer, owned by one process at a time.

The owner's outline, 2026-11-03 evening: go for graphics. By the end of November, Chrome and Firefox with GPU acceleration on the Lenovo and in QEMU, plus simple WebGL games; by the end of 2026, original Crysis under Proton ("from a file system without directories to Proton and Crysis in one year"). Then the owner turns to investment, conferences and promotion while the team stabilizes, secures and cleans up what velocity left. 2027: a stabilization run, ports to SBCs (Raspberry Pi), the SoC the owner is building, and install UX. The owner wants careful research and decomposition up front, and a deliberate choice of when to slow down for building and testing and when to let agents pile on for velocity. See [[graphics-plan]].

**Why:** the owner did not expect agents to make a full OS feasible; the educational-kernel plan was the original market angle.

**How to apply:** weigh work by how much consumer software it unlocks; do not argue for shrinking the kernel for readability; AGENTS.md still calls Rach2 educational, so change that line only when the owner says. Related: [[act-on-velocity]], [[robust-networking]], [[dev-log-treasure]].

## graphics-plan


The owner discussed the graphics stack with Codex (Astra) in the Codex thread "rach2-graphics", id 01a111bc-f616-7d90-871a-759b50e31da9, file `~/.codex/sessions/2026/10/06/rollout-2026-10-06T21-02-38-01a111bc-f616-7d90-871a-759b50e31da9.jsonl` (extract user/assistant `response_item` messages). As of 2026-11-03 it was discussion only; nothing was planned.

What the owner settled there:
* Linux-compatible interfaces at the graphics boundary: DRM/KMS, the i915 uAPI Mesa needs, /dev/dri/card0 and renderD128, dma-buf style sharing, fences, evdev, libseat/seatd.
* First target: fullscreen apps one at a time (browser, games), accelerated, OpenGL/GLES through Mesa; Vulkan and WebGPU deferred on the Haswell Lenovo (HD 4400: Crocus GL 4.6; HASVK Vulkan incomplete).
* Stack: Mesa, Wayland, Weston with kiosk-shell (DRM backend; Pixman renderer first, then GL ES); full Chromium (today only chromium-headless-shell), WebGL 1/2.
* Proton with OpenGL (WineD3D) on the Lenovo, original Crysis DX9 (GOG 64-bit build) as the demo; Vulkan, DXVK and vkd3d-proton later on better hardware. Gaps: x86_64 has no 32-bit compatibility mode or int 0x80; Wine wants a standard audio path.
* Order: virtio-gpu in QEMU first (2D, software Mesa, Weston); then virtio-gpu-gl/VirGL on the testing host (WSLg proved a Linux guest renders on the RTX 4080 SUPER, GL 4.2, SDL over X11, WSL started from the desktop session, GALLIUM_DRIVER=d3d12 and MESA_D3D12_DEFAULT_ADAPTER_NAME=NVIDIA); then native Haswell; then the research experiment.
* Research: a Linux driver VM as a GPU service (KVM-like support in Rach2, VMM, VirGL/Venus protocol, VFIO-like device assignment); design the native stack now with buffer handles, async fences, defined device loss and single device ownership so that experiment plugs in.
* Lab: an iPhone on a tripod via USB Continuity Camera on the Mac to watch the Lenovo's panel, netconsole logs, and a reset method for hangs.
* Team: two main implementers plus a Sonnet helper, Astra reviews at four integration points.

Owner decisions in the chat, 2026-11-03: software rendering is a must, also as the fallback on any machine, "because most decent Op. Systems can do that"; do as much as possible in QEMU first. Also a must, same evening: 32-bit programs on the 64-bit kernel (Linux's compat ABI), for Windows Office apps on Wine and old games. At 21:18 the owner made it explicit, after the chat argued that Wine's new WoW64 needs only a small slice: "I still want proper 32-bit on 64-bit support", meaning the full ia32 compat ABI (32-bit ELF, the i386 call table through int 0x80 and sysenter, compat structures and ioctls, 32-bit signals and vDSO, an i386 multiarch root). The WoW64 slice is its first task. The chat proposed bringing up the Lenovo's panel under the new stack with software rendering early, so the browsers show there before Haswell acceleration lands.

Related: [[project-direction]], [[dev-log-treasure]].

Owner's decision, 2026-11-06: no WINQ-EMU on the testing PC, "looks messy". QEMU on Windows means only the stock build or the test engine's build with our fixes (`tools/setup/qemu-whpx`). Do not propose it again. Same day: no gfxstream either, "sounds also messy". The reference lavapipe is the target for agents on the testing PC, for light graphics programs, in WSL or native Windows, whichever gives the higher velocity (the owner expects WSL); Crysis work is done only on the Lenovo, at the very end.

Per-file ext2 locks and per-file fsync, ordered by the owner on 2026-11-06, went in with milestone 10 (73 wifi steady), so the graphics plan does not carry them as a task.

## graphics-research-active


On 2026-11-03 the owner ordered the chat itself to research, plan and break down the graphics work up to original Crysis under Proton on the Lenovo, with Codex (Astra) and Fable researching too, then Argon (agy) and Muse Wonder Full for second opinions, in two passes (milestones and tasks, then lowest level). The board stays untouched until milestone 71's release finishes. The deliverable is one written document.

**Why:** the owner wants hours of deliberation up front, then maximum velocity.

**How to apply:** after any compaction read `build/flow/coordinator/graphics-research/notes.md` first; it records the step reached and where each file is. Related: [[graphics-plan]], [[project-direction]].

Since 2026-11-05: the next discovery run is being designed. `env/` under the notes directory holds the question of the best agent environment on the testing PC (Fable, Codex, Muse, the chat's position, comparison). The udmabuf module is on hold until the owner says; the owner's to-dos are in notes.md. The run itself is not ordered yet.

On 2026-11-06 the owner approved `run2-plan.md`, the eleven-step second discovery run, as a whole, to start when the Wi-Fi run ends. It covers the udmabuf module and Debian on the Lenovo's trial partition. Run it as the night's run was, from the chat with subagents, on the manager's final message.

## crysis-copies


Unpacked GOG Crysis 2.0.0.7 (8.2 GiB, 2358 files, manifest `game.sha256`) is in `build/cache/crysis/` on the Mac, the same path in WSL's clone, `D:/rach2/build/cache/crysis/` on Windows, and `/opt/crysis/game` on the Lenovo's root partition. `build/cache/crysis/README` says how to use it; `crysis-game.ext2` is attached with `snapshot=on` because the game writes logs into its own tree.

**Why:** the owner's order of 2026-11-05: ready for the next discovery run on both machines, and decisions rest on comparing against Linux in the same environment.

**How to apply:** after any Lenovo root update run `sh build/cache/crysis/push-lenovo.sh`, since the update rewrites /opt. Bin64/Crysis.exe -dx9 is a 64-bit route, so Crysis does not itself need ia32. The fixed benchmark is `Crysis.exe -DEVMODE +map island +exec benchmark_gpu`. Related: [[graphics-plan]], [[graphics-research-active]].

## testing-pc-nested-virt


Since the Windows updates KB5132402 and KB5127936 (installed 2026-11-03), the testing PC's WSL (kernel 6.18.54.1) has nested virtualization: `svm` on all 32 threads, kvm_amd `nested=1`, `/dev/kvm` root:kvm 660, and QEMU runs guests with `-accel kvm` as the user `testing`. A guest inside such a guest (level 3) is very slow on exits: Linux booted in 60-71 s under KVM against 2.9 s under TCG (measured 2026-11-04, `graphics-research/tonight/qemu-wsl.md`).

On the owner's order of 2026-11-04 04:10 the seven test accounts joined the `render` group of the testing PC's Ubuntu. WSL's kernel has no CONFIG_UDMABUF, so Venus over lavapipe there stops at Vulkan 1.2 without a swapchain.

**Why:** the graphics plan (draft 5) and any test-speed work depend on these; the plan of draft 4 still said "no nested virtualization".

**How to apply:** do not assume TCG-only on the testing PC; keep TCG for exit-heavy third-level runs; a WSL kernel with udmabuf is an owner decision (plan's decision A). See [[testing-pc-root-access]].
