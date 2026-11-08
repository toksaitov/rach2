# Agent environments on the testing PC: comparison of the four answers

Written 2026-11-06 from `fable.md`, `codexwork/report.md`, `muse.md` and `position-chat.md`. Checked by the chat against the fork's GitHub state.

## Where all four agree

* WSL stays where agents live, and QEMU in WSL with virgl on the RTX stays the daily bed for OpenGL and WebGL.
* A Hyper-V Linux VM with a GPU partition is WSL's own path again and adds nothing for graphics.
* DDA is outside what Microsoft supports on Pro; the only client report ends in 0xC035001E.
* Rach2 as a Hyper-V guest, VMware and VirtualBox are out: each needs a new driver family and has no guest Vulkan.
* Lavapipe inside the guest is the control for correctness; Venus on host lavapipe tests the transport; neither says anything about a GPU.
* Agents share a GPU at process level. A host GPU reset (TDR) hits every user of that GPU, including WSL.
* gfxstream is the second candidate on Windows if Venus fails there.

## Checked this round

* WINQ-EMU exists and is real source: Fable and Codex each read it; the chat read the clone and GitHub. Muse's "does not exist today" is wrong; Muse did not find it.
* Its renderer fork's last commit is 2026-04-27 (alpha10). Later commits are only to the launcher (issues 6 and 7, September).
* Issue 2, open since 2026-08-18: RTX 3070 Ti, Ubuntu 26.04 guest, vkcube freezes the guest. It is the only NVIDIA report found, and it is a failure. Issue 1, open: q35 black screen.
* So Fable's "claimed for NVIDIA without evidence" understates it: there is evidence, and it is against. Whether it is NVIDIA, the guest or the launcher's flags is unknown.

## Where they disagree

1. udmabuf. Fable and the chat: a module with no WSL restart, after one kernel build for its symbols. Codex: the module is research; prefers a whole kernel through `.wslconfig` and a WSL restart. Muse assumed the whole kernel.
2. The real-GPU lane. Fable, Codex and the chat: Venus on Windows, a second Linux machine only if it fails. Muse: buy the Linux machine first; port only if refused.
3. DDA. Muse: a controlled attempt on the RTX in a maintenance window. Fable: last, on the Radeon. Codex: not on this machine; read-only survey only.
4. A deliberate GPU hang. Muse and the chat: test it. Codex: never on the desktop card. Fable: alone, if at all.
5. A plain Hyper-V Linux VM with no GPU as the lavapipe host: only Codex proposes it, as the route if WSL must stay untouched.
6. Dozen behind Venus: Muse wants a cheap vtest; Fable's reading of Mesa 25.0.7 says 1.2 and no fence fds, a curiosity; Codex says last.
7. `-cpu`: Fable uses `-cpu host` with the fork; Codex keeps `max` first and tests `host` separately.

## New requirement from the owner

Decisions rest on what can be compared against Linux in the same environment, to prove that a thing is possible. Unpacked Crysis is ready, with a fixed route (`Crysis.exe -DEVMODE +map island +exec benchmark_gpu`), a 64-bit DX9 route and an ext2 disk for guests. None of the four answers designed its run around that.

## After the argument round (2026-11-06 00:10)

Answers: `fable-round2.md`, `codex-round2.md`, `muse-round2.md`.

Settled, all agreeing unless noted:

* udmabuf: the module first, after a read-only check and one exact-tag kernel build. Codex and Muse came over. Switch to a whole kernel through `.wslconfig` if the module will not load after an exact rebuild, if the vmap change cannot be shown safe, or when the bed becomes a release fixture. Load it only when no WSL job is running: a module fault stops every WSL user.
* WINQ-EMU: build alpha10 as it is, pinned, in a private prefix, no rebase. Keep `-cpu max` first, `host` as a variant. Probe in steps so that a freeze names its cause: vulkaninfo, memory-type map and readback, a fence-only render, then vkcube.
* If it freezes on the RTX, the same build on the Radeon next. Radeon passes: NVIDIA-specific, try hiding memory type 4. Both fail: gfxstream on Windows next, and the second machine goes to the owner. Muse wants that in parallel, Fable after gfxstream, Codex when neither Windows transport passes; followed: put to the owner when both GPUs fail.
* DDA, a deliberate GPU hang: out of the run. Plain Hyper-V lavapipe host: only if both WSL routes are refused.
* Dozen: Fable and Muse out, Codex a bounded check. Set aside: no milestone needs Vulkan 1.2 behind Venus; Codex's point stands that the reading was of Mesa 25.0.7.
* Crysis on the CPU rows: Muse leaves it off, Fable and Codex keep it. Followed: one pass on Venus over lavapipe, since it proves a real game crosses the transport.
* Rows the three left out, added by the chat: the Lenovo's bare Linux under GE-Proton. The night's Alpine is musl and cannot run Proton, so that session needs a glibc system.

## The owner's decision of 2026-11-06

No WINQ-EMU: "looks messy". Windows QEMU means only the stock build or the test engine's build with our fixes. That takes steps 5 and 6 out of the run, and gfxstream with them, since it needs a build of its own. Vulkan on a real GPU is then not reachable on the testing PC; the second Linux machine is the only other route, and it is the owner's money.

## The owner's second decision of 2026-11-06

No gfxstream: "sounds also messy". The reference lavapipe is the target for agents on the Windows PC, for light graphics programs. Crysis work is done only on the Lenovo, at the very end.

Lavapipe inside the guest needs no host graphics, so the test engine's QEMU can carry it as it stands: `D:/rach2/build/qemu` has virtio-vga, virtio-gpu-pci, VGA, bochs-display and ramfb, with tcg and whpx (read from `-device help`). Not yet run.
